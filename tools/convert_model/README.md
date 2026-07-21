# 模型转换工具（x86 TPU-MLIR）

本目录提供 best.onnx -> BM1684 bmodel 的转换脚本与校准数据准备。
**转换须在 x86 开发机执行**（工控机为 BM1684 SoC，无 TPU-MLIR，按约束不得安装）。

## 文件

| 文件 | 用途 |
|---|---|
| `extract_calibration.sh` | 从 `testdata/test.mp4` 抽取 INT8 校准帧（真实数据，工控机/x86 均可运行） |
| `convert_bmodel.sh` | x86 TPU-MLIR 转换管线：默认 ONNX -> MLIR -> BM1684 F32 bmodel；INT8 须显式启用 |

## 前置条件（x86）

- TPU-MLIR 官方固定版本镜像（容器内 `model_transform.py`、`model_deploy.py`、`model_tool` 可用）；
- 仓库已含 `hangzhouwan_beishang/weights/best.onnx` 与至少一张本地验证图片（默认从 `testdata/calibration/` 选择）。

## 转换参数（来自 ONNX 审计，见 `docs/06`）

| 项 | 值 | 来源 |
|---|---|---|
| 输入名 | `images` | `tools/inspect_onnx.py` |
| 输出名 | `output` | 同上（模型不含 NMS，后处理在 C++ 侧） |
| 输入形状 | `[[1,3,640,640]]` | 原模型动态 `[batch,3,height,width]`，按 detector.py 640×640 固化 |
| 处理器 | `BM1684` | 不得用 BM1684X |
| 模式 | `F32` 基线 -> `INT8` 校准 | 不得 FP16（BM1684 不支持） |
| 校准集 | `testdata/calibration/*.jpg`（63 帧，960×544） | `extract_calibration.sh` 从 test.mp4 派生 |
| 预处理 | resize 640×640 / RGB / mean=0 / scale=1/255 | 与原 `detector.py` 一致；bmodel 仅做纯推理 |

## 执行步骤

```bash
# 在 x86 TPU-MLIR 容器内执行。默认只生成 F32，不会生成 INT8。
MODE=f32 bash tools/convert_model/convert_bmodel.sh
# 产物：artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel
```

## 转换后（板端 BM1684 验证）

```bash
# Sophon 自带工具：板端 bmrt_test 加载/推理验证
/opt/sophon/libsophon-current/bin/bmrt_test --bmodel weights/yolov7_ship_1684_f32.bmodel --devid 0

# 或自研最小加载器（见 tools/image_inference/）
LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib ./tools/image_inference/bmrt_load_test weights/yolov7_ship_1684_f32.bmodel
```

## 约束提醒

- 目标必须 `BM1684`，**不得** `BM1684X`；**不得** FP16 模式；
- 不得在工控机安装 TPU-MLIR / torch / onnxruntime-gpu / CUDA；
- F32 基线须先板端验证精度通过，再验证 INT8 精度（与 F32/原 ONNX 对比）。
