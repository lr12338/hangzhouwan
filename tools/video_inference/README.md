# -*- coding: utf-8 -*-
# BM1684 单路视频硬件推理工具

## 简介

`single_video_infer` 是阶段4交付的单路视频硬件推理 CLI：

```
test.mp4 → h264_bm 硬解 → 容量1丢旧队列 → 间隔推理(复用snapshot) → 绘框
         → in-place 写回 NV12 → h264_bm 硬编 → 本地输出文件
```

- 硬件解码：Sophon-FFmpeg C API + `h264_bm`（libavcodec）
- 硬件编码：Sophon-FFmpeg C API + `h264_bm`（libavcodec）
- 推理：复用阶段3 `BmrtDetector`（模型只加载一次）
- 颜色转换与缩放：libswscale（SIMD 优化，绕开 CPU `resize_bilinear` 瓶颈）
- 绘框：复用阶段3 `hzw::Image::draw_rectangle` / `draw_label`
- 队列：容量1、主动丢旧帧、线程安全
- 结果复用：`DetectionSnapshot` + TTL，过期不绘制

## 编译

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

## 运行

```bash
export LD_LIBRARY_PATH=/opt/sophon/sophon-ffmpeg_0.8.0/lib:/opt/sophon/libsophon-0.4.9/lib
./build/single_video_infer \
  --input testdata/test.mp4 \
  --output artifacts/stage4/output_single_stream.mp4 \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --device 0 --decoder h264_bm --encoder h264_bm \
  --output-fps 10 --inference-fps 5 --bitrate-kbps 800 \
  --queue-size 1 --conf 0.1 --iou 0.1 --loop 1
```

## 参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `--input` | `testdata/test.mp4` | 输入 mp4 路径 |
| `--output` | `artifacts/stage4/output_single_stream.mp4` | 输出路径（mp4/ts/h264） |
| `--bmodel` | `artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel` | bmodel 路径 |
| `--device` | `0` | BM 设备号 |
| `--decoder` | `h264_bm` | 解码器名称 |
| `--encoder` | `h264_bm` | 编码器名称 |
| `--source-fps` | `20` | 源帧率（用于实时节流与帧率校验） |
| `--output-fps` | `10` | 输出帧率 |
| `--inference-fps` | `5` | 推理频率 |
| `--bitrate-kbps` | `800` | 编码目标码率 |
| `--gop` | `20` | I 帧间隔 |
| `--queue-size` | `1` | 队列容量（推荐1） |
| `--conf` | `0.1` | 置信度阈值 |
| `--iou` | `0.1` | NMS IoU 阈值 |
| `--result-ttl-ms` | `1000` | 检测结果复用有效期 |
| `--loop` | `1` | 循环次数（0=无限） |
| `--max-seconds` | `0` | 最大运行时长（0=不限） |
| `--metrics-interval` | `10` | 指标汇总输出间隔（秒） |

## 帧率约束

`source_fps` 必须能被 `output_fps` 和 `inference_fps` 整除，且 `inference_fps ≤ output_fps ≤ source_fps`。默认 20/10/5 满足：20%10==0、20%5==0、10%5==0。非法组合会被拒绝。

## 信号处理

响应 `SIGINT`（Ctrl+C）与 `SIGTERM`，优雅停止（关闭队列 + 释放资源）。
