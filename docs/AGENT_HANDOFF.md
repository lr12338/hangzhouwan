# Agent 接手提示（AGENT_HANDOFF）

## 当前状态：2026-07-21

项目根目录为 `/home/linaro/hangzhouwan-orign/hangzhouwan`，分支为 `feat/bm1684-edge-deployment`。目标芯片固定为 BM1684（`chipid=0x1684`），已完成 F32 bmodel 板端验证和单图 C++ 推理 PoC。

## 已完成

### 阶段2：F32 bmodel 转换与 x86 验证
- F32 bmodel 已生成：`artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel`（24,428,544 字节）
- Git 提交 `9255f27`（bmodel 已跟踪，`git ls-files` 确认）
- SHA256：`d1c295c504888541c27e20fda500976725b9d13b6ef5c91842f247f52c31cfcd`
- ONNX/MLIR 172 个 Tensor 数值验证通过；bmodel cmodel 比较通过

### 阶段2B：板端 bmodel 真实加载
- `bmrt_test` 加载成功（退出码 0）
- `bmrt_load_test` 编译运行成功（退出码 0）
- 网络 `yolov7_ship`：输入 `images [1,3,640,640] FLOAT32`，输出 `output_Concat [1,25200,6] FLOAT32`
- 输出名动态读取（非硬编码 `output`）
- 无兼容性错误，TPU 资源正常释放

### 阶段3：单图 C++ 推理 PoC
- 代码结构：`include/inference/` + `src/inference/` + `include/image_io/` + `src/image_io/`
- 预处理：libjpeg 解码(RGB) → 直接 resize 640×640 → /255 → NCHW FLOAT32（与 Python 一致）
- 后处理：obj_conf 过滤 → score 过滤 → 坐标映射 → NMS（与 detector.py 一致）
- 单元测试：10 项全部通过
- 单图推理：frame_30.jpg 检测到 2 艘船（预处理 56ms，推理 16ms，后处理 0.6ms）
- 100 次重复推理：平均 15.99ms，框数稳定，无内存泄漏
- Python 测试：26 项全部通过

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure

# 单图推理
LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib \
  ./build/single_image_infer \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --image testdata/model_test/frame_30.jpg \
  --output result.jpg --json result.json \
  --conf 0.1 --iou 0.1 --device 0

# 100 次重复推理
LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib \
  ./build/single_image_infer \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --image testdata/model_test/frame_30.jpg \
  --conf 0.1 --iou 0.1 --device 0 --loop 100
```

## 精度声明

板端 F32 单图推理、后处理和绘框功能已通过。**最终检测框精度仍需与同图 ONNX 基线 JSON 对照**。当前无 x86 基线 JSON。

## 待完成

- 与 x86 ONNX 基线 JSON 进行精度对照
- 阶段4：单路硬件视频管线 PoC（RTSP）
- 后续：INT8 量化、AIS 坐标映射、双路 Pipeline、部署

## 执行红线（始终遵守）

禁止：安装 TPU-MLIR、Python SAIL、Torch/CUDA、修改 libsophon、INT8 转换、连接生产 RTSP/MQTT/RTMP、修改 systemd、硬编码输出名为 `output`、伪造测试结果。
