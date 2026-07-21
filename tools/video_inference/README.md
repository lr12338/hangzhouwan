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
- 颜色转换：BMCV `storage_convert`（NV12->RGB CSC，约 0.3ms）或 libswscale
- 缩放：libswscale（BMCV VPP resize 在本板不可用）
- 绘制：BMCV `draw_rectangle`（约 0.4ms）或 CPU `draw_rectangle` / `draw_label`
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
| `--preprocess` | `cpu` | 预处理路径：`cpu`（sws，检测正确）或 `bmcv`（BMCV CSC，性能对比） |
| `--draw-mode` | `cpu` | 绘制路径：`cpu`（sws 往返）、`bmcv`（BMCV draw_rectangle，推荐）、`none`（不绘制） |

## 帧率约束

`source_fps` 必须能被 `output_fps` 和 `inference_fps` 整除，且 `inference_fps ≤ output_fps ≤ source_fps`。默认 20/10/5 满足：20%10==0、20%5==0、10%5==0。非法组合会被拒绝。

## 信号处理

响应 `SIGINT`（Ctrl+C）与 `SIGTERM`，优雅停止（关闭队列 + 释放资源）。

## BMCV 双路径（阶段4 性能优化）

推荐配置（10fps + 检测正确性）：
```bash
./build/single_video_infer ... --preprocess cpu --draw-mode bmcv
```

| 配置 | fps | 检测正确性 | 说明 |
|------|-----|-----------|------|
| `--preprocess cpu --draw-mode bmcv` | ~10 | ✅ 与基线一致 | 推荐：CPU 检测 + BMCV 快速绘制 |
| `--preprocess bmcv --draw-mode bmcv` | ~10 | ⚠️ IoU 0.94–0.98 | BMCV CSC 系数差异，仅性能对比 |
| `--preprocess cpu --draw-mode cpu` | ~5 | ✅ | 原始路径（sws 往返瓶颈） |
| `--preprocess cpu --draw-mode none` | ~10 | ✅ | 不绘制，测纯管线性能 |

详见 `docs/19-stage4-bmcv-performance-optimization.md`。

## 单路 RTSP 输入（阶段4.2）

`--source-type rtsp` 从 RTSP 实时流读取，复用同一 h264_bm 硬解 -> 推理 -> BMCV 绘框 -> h264_bm 硬编链路，输出本地 MP4/TS。RTSP 模式不做墙钟节流（由网络按真实速率到达）。

**安全**：RTSP URL **必须**经环境变量 `--input-env` 传入，禁止 `--input 'rtsp://user:password@...'`（会进入 shell 历史/进程列表/日志）。日志自动脱敏为 `rtsp://user:***@host`。

```bash
export HZW_TEST_RTSP_URL='rtsp://用户名:your_password@测试地址:554/路径'   # 板端私下设置，勿写入文件/命令行
./build/single_video_infer \
  --source-type rtsp --input-env HZW_TEST_RTSP_URL \
  --output artifacts/stage4_2/rtsp_out.mp4 \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --device 0 --decoder h264_bm --encoder h264_bm \
  --rtsp-transport tcp --rtsp-stimeout-us 5000000 --rtsp-max-reconnect -1 \
  --source-fps 20 --output-fps 10 --inference-fps 5 --queue-size 1 \
  --preprocess cpu --draw-mode bmcv --max-seconds 60
```

| 参数 | 默认 | 说明 |
|------|------|------|
| `--source-type` | `file` | `file` 或 `rtsp` |
| `--input-env` | - | RTSP 必填：输入 URL 的环境变量名（URL 不入命令行/日志） |
| `--rtsp-transport` | `tcp` | `tcp` / `udp`（板端 `ffmpeg -h demuxer=rtsp` 实测支持） |
| `--rtsp-stimeout-us` | `5000000` | socket TCP I/O 超时（微秒），覆盖连接与读取超时 |
| `--rtsp-max-reconnect` | `-1` | 断线重连次数：`-1`=无限 `0`=不重连 `>0`=上限 |
| `--rtsp-initial-backoff-ms` | `1000` | 初始退避 |
| `--rtsp-max-backoff-ms` | `30000` | 指数退避上限 |

特性：
- 连接/读取超时：`stimeout`（TCP）；无数据到达时 `av_read_frame` 在超时后返回错误触发重连。
- 受控断线重连：指数退避（封顶 30s），受 `max_reconnect` 约束；重连成功后清除过期检测结果、输出 PTS 保持单调；bmodel 不重新加载。
- 停止信号中断阻塞：`SIGINT`/`SIGTERM`/限时经中断回调使阻塞的 open/read 返回 `Immediate exit requested`。
- 初始连接失败不重连（直接退出 3），避免对坏地址无限重连。

详见 `docs/21-stage4-2-single-rtsp-input.md`、`docs/22-stage4-2-manual-rtsp-stability.md`。
