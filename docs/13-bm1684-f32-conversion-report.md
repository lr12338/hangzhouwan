# 13 · BM1684 F32 bmodel 转换报告

检查时间：2026-07-21。目标芯片固定为 BM1684（`chipid=0x1684`），不是 BM1684X；本次只允许 F32，未执行 FP16、INT8、板端加载或交付复制。

## 已完成的离线检查

| 项目 | 实测值 |
|---|---|
| 分支 / HEAD | `feat/bm1684-edge-deployment` / `b7a9fbc` |
| 归档 SHA256 | `d83e2cb55bc279076e516597363429c9bab76aae7999260046a786220b4819ac` |
| 实际导入镜像 | `sophgo/tpuc_dev:v3.4`，别名 `local/tpuc_dev:v3.4` |
| 镜像 ID | `sha256:d73afc9614a7e78e69dca61a9b7ac84ee9598c942571ea557c603850c5df59a1` |
| 镜像属性 | `amd64` / `linux` |
| 根分区可用空间 | `docker load` 后立即为 9.4 GB；最终复核为 16 GB |
| 受控 ONNX | `weights/best.onnx` 不存在，故无可报告的受控源 SHA256 |
| 其他发现的 ONNX | `hangzhouwan_beishang/weights/best.onnx` 存在，但不是本次要求的输入路径 |
| 测试输入 | 没有图片；`testdata/test.mp4` 存在，但主机及容器都无 `ffmpeg` |

## 脚本和预处理复核

`tools/convert_model/convert_bmodel.sh` 已通过 `bash -n`。默认 `MODE=f32`，含 `set -euo pipefail`，使用 `--processor BM1684`、`--input_shapes [[1,3,640,640]]`、输入 `images`、输出 `output` 和 `--quantize F32`。脚本仅在显式指定 `MODE=int8` 时才会进入 INT8 分支，本次没有执行该分支；脚本没有 BM1684X 或 FP16 参数。

原 `hangzhouwan_beishang/detector.py` 的预处理为：OpenCV BGR 转 RGB，直接 resize 到 640x640，除以 255，转 NCHW 并增加 batch 维度。未使用 letterbox，因此未擅自加入 `--keep_aspect_ratio`。

## 阻塞与执行状态

转换未开始，未生成 `artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel`。原因按门禁顺序如下：

1. `docker load` 结束后根分区曾只剩 9.4 GB，低于要求的 10 GB；最终复核为 16 GB。任何重试前必须重新检查，不能依赖早先读数。
2. `local/tpuc_dev:v3.4` 内没有发现 TPU-MLIR 工具、wheel、源码安装线索或环境脚本，不能确认并运行 BM1684 转换。
3. `weights/best.onnx` 不存在；应由用户手动复制模型到该路径，不能以不同路径的模型替代。
4. 没有稳定测试图片，且无法用现有工具从视频抽帧。

因此没有 bmodel 大小或 SHA256、模型结构信息、ONNX 参考输出、F32 模拟输出、数值比较日志、模型清单或交付包。没有将原始 Tensor 一致性误述为检测框或 NMS 精度。

## 继续条件

1. 释放或扩容空间，使镜像导入后根分区至少保留 10 GB。
2. 补充含 TPU-MLIR 工具和官方环境脚本的镜像，或提供镜像内的官方安装说明和 wheel。
3. 手动将 `best.onnx` 放到 `weights/best.onnx`，并提供一张 JPG/JPEG/PNG 测试图片。

最终结论：**镜像内 TPU-MLIR 工具不可用，需补充官方环境。**
