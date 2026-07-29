# 06 · 阶段2 ONNX 审计与 bmodel 转换准备

> 阶段1 已确认通过；本阶段在模型补齐后执行 ONNX 审计 + bmodel 转换准备。
> 依据：阶段2 目标 = 审计 best.onnx；x86 TPU-MLIR(匹配0.4.9) 转 BM1684 F32->INT8；
> 真实数据校准；门禁 = 板端 bmrt_test/最小 C++ 加载成功、精度通过。

## 1. 阶段目标与边界

- 审计 `best.onnx` 结构（输入/输出/算子/动态维度/转换就绪）；
- 准备 INT8 真实数据校准集（从 `testdata/test.mp4` 派生）；
- 编写 x86 TPU-MLIR 转换脚本（F32 基线 -> INT8，目标 `BM1684`，无 FP16）；
- 板端构建并验证最小 C++ BMRuntime 加载程序；
- 文档化板端 `bmrt_test` 用法。
- 约束：不得安装 TPU-MLIR/torch/onnxruntime-gpu/CUDA/SAIL；不得在工控机转换 bmodel；
  不得接正式服务；不得改 systemd。bmodel 转换为 x86-only（工控机为 SoC，无 TPU-MLIR）。

## 2. 模型资产确认（已补齐）

| 资产 | 大小 | SHA256 | 状态 |
|---|---|---|---|
| `best.onnx`（检测） | 24,133,596 B | `101f8e19c680eb4fd2446521f983b11ada35052bce56f30c2ddcf5df64894f92` | 存在·已审计 |
| `0121_random_forest_model.pkl`（A 路坐标） | 7,595,545 B | `d25e7d0ed18474c60021b4199a254ada286fc74f79459ff279c7530ddb0d44f8` | 存在（坐标迁移属阶段5） |
| `beishang_x-l.pkl`（B 路坐标） | 2,147,673 B | `f85934c48f4146d0d572676a47b09d1c3f206abe2b00e1662c50b2caf200f48a` | 存在（坐标迁移属阶段5） |
| `beet0110.pt`（原 PyTorch 权重） | 74,775,738 B | `3ea33fd372b43935d4eefff978c0ce0121158ae85981dd33a8c3a42eba344b12` | 存在（仅参考，不进 BM1684 生产） |

> 四个文件均被 `.gitignore` 排除（`*.onnx`/`*.pkl`/`*.pt`），未入库，仅本地 `weights/`。

## 3. ONNX 审计结果（`tools/inspect_onnx.py`，纯标准库 protobuf 解析）

工控机未装 `onnx` 且 PyPI 网络超时，故按 protobuf 线格式自研解析器（`tools/inspect_onnx.py`），
不安装任何禁止依赖。审计 `best.onnx`：

| 项 | 值 |
|---|---|
| ir_version | 7 |
| producer | pytorch（2.3.1） |
| opset_import | ai.onnx=12 |
| graph 名称 | main_graph |
| 节点数 / 初始化张量数 | 239 / 116 |
| 输入 | `images` : FLOAT **[batch,3,height,width]**（动态） |
| 输出 | `output` : FLOAT **[batch,Concatoutput_dim_1,y]**（动态） |

算子统计（均为 TPU-MLIR 常见支持算子）：
`Conv×58 LeakyRelu×55 Constant×37 Concat×24 Unsqueeze×12 Gather×9 Shape×9 MaxPool×6 Mul×6 Reshape×6 Add×3 Pow×3 Sigmoid×3 Split×3 Transpose×3 Resize×2`

**关键结论：**
1. **输入动态维度** `[batch,3,height,width]` -> 转换时须固化 `[1,3,640,640]`（与 `detector.py` 的 640×640 一致），输出维度随之静态化；
2. **输出名 `output`**（非 `score`/`batchno_classid_x1y1x2y2`）-> 模型**不含 NMS/后处理**，后处理（xywh->xyxy、NMS、rescale）须在 C++ 侧实现（阶段3）；
3. 算子集合为 YOLOv7 典型，预期 TPU-MLIR 可处理（最终以 x86 转换实测为准）。

## 4. INT8 校准数据准备

`tools/convert_model/extract_calibration.sh` 用 Sophon-FFmpeg 从 `testdata/test.mp4` 抽帧：

- 每 2 秒 1 帧，共 **63 帧**，960×544 JPEG，4.2 MB；
- 抽取过程启用 Sophon 硬件解码（`BMvidDec` / `chagall_dec.bin`）；
- 校准帧为派生产物，已 gitignore（`testdata/calibration/*.jpg`），可由脚本 + test.mp4 再生；
- 预处理与原 `detector.py` 一致：resize 640×640 / RGB / mean=0 / scale=1/255。

## 5. x86 TPU-MLIR 转换方案（`tools/convert_model/convert_bmodel.sh`）

须在 x86 执行（工控机无 TPU-MLIR，且约束禁止安装）。管线：

1. `model_transform`：ONNX -> MLIR，`--input_shapes [[1,3,640,640]]` 固化动态输入，`--input_names images --output_names output`；
2. `model_deploy --processor BM1684 --mode F32`：生成 F32 基线 bmodel；
3. `run_calibration`：用 63 帧真实数据生成 INT8 校准表；
4. `model_deploy --processor BM1684 --mode INT8 --calibration_table`：生成 INT8 生产 bmodel。

产物：`weights/yolov7_ship_1684_f32.bmodel`（基线）、`weights/yolov7_ship_1684_int8.bmodel`（生产）。
强调：处理器必须 `BM1684`（**不得 BM1684X**），模式 F32->INT8（**不得 FP16**）。

## 6. 板端 C++ 加载验证（阶段2 门禁：最小 C++ 加载）

`tools/image_inference/bmrt_load_test.cpp`（最小 BMRuntime 加载程序）：

- 编译通过：`g++ -std=c++14 -I/opt/sophon/libsophon-0.4.9/include ... -lbmrt -lbmlib`；
- 板端运行（无 bmodel）实测：成功打开 BM 设备 0，BMRuntime CPU op 库（`libcpuop.so`）加载，API 链路正常，退出码 0；
- 芯片复核：`chipid=0x1684 model=BM1684`（与阶段0 一致）。

板端 `bmrt_test`（Sophon 自带）已就位：
`/opt/sophon/libsophon-current/bin/bmrt_test --bmodel <bmodel> --devid 0`。

> 最小 C++ 加载程序的“加载真实 bmodel 并打印网络形状”须在 x86 转换出 bmodel 后执行，
> 即满足“最小 C++ 加载成功”门禁的最后一步。

## 7. 约束遵守情况

| 约束 | 遵守 |
|---|---|
| 目标 BM1684（非 BM1684X） | 是（转换脚本 `--processor BM1684`，芯片复核 0x1684） |
| F32->INT8，不得 FP16 | 是（脚本仅 F32/INT8，无 FP16） |
| 不得安装 TPU-MLIR | 是（未安装；转换脚本标注 x86 执行） |
| 不得在工控机转换 bmodel | 是（未转换；仅准备脚本） |
| 不得安装 torch/onnxruntime-gpu/CUDA/SAIL | 是（ONNX 审计用自研标准库解析器，未装任何禁止依赖） |
| 不得接正式服务 / 改 systemd | 是（仅本地 test.mp4 抽帧与 C++ 编译） |

## 8. 阶段门禁结论

**阶段2 板端准备完成；bmodel 转换与板端精度验证待 x86 执行。**

已完成（板端）：
- ONNX 审计完成（动态输入固化方案、无内置 NMS 结论、算子就绪评估）；
- INT8 真实数据校准集就绪（63 帧）；
- x86 TPU-MLIR 转换脚本就绪（F32->INT8，BM1684）；
- 最小 C++ BMRuntime 加载程序编译并验证 API 链路；
- 板端 `bmrt_test` 就位。

待执行（x86，使用本阶段脚本）：
- 运行 `convert_bmodel.sh` 产出 F32/INT8 bmodel；
- 将 bmodel 拷回板端 `weights/`。

待执行（板端，bmodel 到位后）：
- `bmrt_test --bmodel ..._1684_f32.bmodel` 与 `bmrt_load_test ..._1684_f32.bmodel` 验证加载（满足“最小 C++ 加载成功”门禁）；
- F32 精度与原 ONNX 对比通过 -> INT8 精度验证。

> bmodel 转换为 x86-only（TPU-MLIR 不在 SoC 且约束禁止安装），属平台固有依赖，
> 非本阶段工作缺失。待 x86 转换 + 板端加载/精度验证通过后，阶段2 门禁完全闭合，可进入阶段3（单图 C++ PoC）。
