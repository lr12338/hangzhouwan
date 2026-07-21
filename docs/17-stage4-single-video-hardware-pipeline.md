# -*- coding: utf-8 -*-
# 阶段4：BM1684 单路硬件视频推理管线

## 目标

使用 Sophon 硬件 H.264 编解码建立单路连续视频 Pipeline，复用 BmrtDetector 完成间隔推理，非推理帧复用最近检测结果，实现容量为 1 的低延迟队列和主动丢旧帧策略。

## 运行环境

- **板端**：BM1684 SoC（ARM Cortex-A53）
- **Sophon-FFmpeg**：4.1.3-sophon-0.8.0（`/opt/sophon/sophon-ffmpeg_0.8.0/`）
  - 运行时库：`libavcodec.so.58.35.100`、`libavformat.so.58.20.100`、`libavutil.so.56.22.100`、`libswscale.so.5.3.100`
  - 开发头文件：本机无头文件，使用 Ubuntu focal 的 `libavformat-dev` 4.2.7 头文件（同 major 版本，ABI 兼容），安装至 `/opt/sophon/sophon-ffmpeg_0.8.0/include`
  - 关键注意：FFmpeg 4.x 头文件已移除 `extern "C"` 守卫，C++ 必须显式包裹（见 `include/video/ffmpeg_compat.h`）
- **libsophon**：0.4.9（BMCV + BMRuntime）
- **bmodel**：`artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel`（SHA256 `d1c295c504888541c27e20fda500976725b9d13b6ef5c91842f247f52c31cfcd`）

## 输入视频

`testdata/test.mp4`：
- 编码：H.264（`h264_bm` 硬解）
- 尺寸：960×544
- 像素格式：yuv420p（解码输出 NV12，cbcr_interleave=1）
- 帧率：20fps
- 时长：125.35s（约 2 分钟）
- 码率：1068 kbps

## 硬件编解码能力核验

### 解码器 `h264_bm`

```text
ffmpeg -hide_banner -decoders | grep h264_bm
  V..... h264_bm              bm H.264 decoder wrapper (codec h264)

ffmpeg -hide_banner -h decoder=h264_bm
  -output_format     <int>     uncompressed(0) or compressed(101) (default 0)
  -cbcr_interleave   <int>     (default 1) -> NV12
  -zero_copy         <flags>   (default 1) -> mmap'd device memory, host readable
  -extra_frame_buffer_num <int> (default 2) -> 必须经 AVDictionary 传入
  -sophon_idx        <int>     device index (default 0)
```

经 C API 实证：
```cpp
const AVCodec* dec = avcodec_find_decoder_by_name("h264_bm");
// 输出：dec=h264_bm  ✓
AVFrame 接收：format=23(AV_PIX_FMT_NV12), width=960, height=544,
//              linesize=[960,960,0], data[0]/data[1] 主机可读 (mmap ION)
```

### 编码器 `h264_bm`

```text
ffmpeg -hide_banner -encoders | grep h264_bm
  V..... h264_bm              BM H.264 encoder (codec h264)

ffmpeg -hide_banner -h encoder=h264_bm
  Supported pixel formats: bmcodec yuv420p nv12
  -is_dma_buffer     <flags>   (default 1) -> 输入需为 bm_image
  -preset            <int>     0=fast,1=medium,2=slow (default 2)
  -qp                <int>     constant QP (default -1)
  -enc-params        <string>  override config
```

经 C API 实证：
- 编码器接受解码 AVFrame（承载 bm_image），`avcodec_send_frame` 返回 0=Success
- 不接受 `av_frame_get_buffer` 分配的主机帧（"Invalid pic data!"）
- 结论：输入必须为解码产物的 bm_image AVFrame

### In-place 修改验证

解码帧的 `data[0]`（Y）和 `data[1]`（UV）为 mmap'd 设备内存，CPU 写入可见于 VPU 编码器（无缓存一致性问题）：

```cpp
// 解码 -> memset(data[0], 255) -> 编码
// ffprobe 输出帧 Y 通道：mean=255.0, min=255, max=255 ✓
```

## Pipeline 架构

```
test.mp4
  ↓ avformat_open_input (libavformat)
SophonVideoSource
  ↓ h264_bm 解码（libavcodec + bm_image）
  ↓ 实时节流（按源帧率 20fps 回放）
  ↓ 抽帧（每 2 帧保留 1 帧 → 10fps 输出）
LatestFrameQueue<VideoFrame> (容量1, 丢旧)
  ↓
SingleStreamPipeline::process_loop
  ↓ sws_scale NV12(960x544) → RGB(960x544)  [绘框]
  ↓ sws_scale NV12(960x544) → RGB(640x640)  [推理，跳过 CPU resize_bilinear]
  ↓ HWC→CHW + /255
BmrtDetector::infer (NCHW FLOAT32)
  ↓
DetectionSnapshot (TTL=1000ms)
  ↓ 复用最近结果绘框
draw_rectangle / draw_label
  ↓ sws_scale RGB(960x544) → NV12(960x544)  [in-place 写回解码帧]
LatestFrameQueue<VideoFrame> (容量1, 丢旧)
  ↓
SophonVideoSink
  ↓ h264_bm 编码（libavcodec + bm_image）
  ↓ avformat_write_header / av_interleaved_write_frame
output.mp4
```

### 线程模型

三线程 + 两级容量1丢旧队列：

| 线程 | 职责 | 速率 |
|------|------|------|
| decode_loop | 读 packet → h264_bm 解码 → sws/抽帧 → 推送 | 20fps（实时节流） |
| process_loop | NV12↔RGB/推理/绘框/写回 | ~10fps（BMCV 绘制优化，`--draw-mode bmcv`） |
| encode_loop | h264_bm 编码、封装输出 | 跟随 process |

BmrtDetector 只创建一次（在 run() 中），所有线程复用。

### 帧率调度

```text
source_fps=20, output_fps=10, inference_fps=5
output_step = source_fps / output_fps = 2   → 每 2 帧输出一帧
infer_step  = source_fps / inference_fps = 4 → 每 4 帧推理一次
```

- 解码线程按 `seq % output_step == 0` 过滤输出帧
- 处理线程按 `seq % infer_step == 0` 决定是否推理
- 推理帧必为输出帧（因 `output_step | infer_step`）

### 队列策略

`LatestFrameQueue<T>` 模板：
- 容量固定（默认 1）
- `push` 时若满，丢弃最旧帧（调用 releaser 释放资源）
- `pop` 阻塞；`close()` 唤醒消费者，空队列返回 false 退出
- 线程安全（mutex + condition_variable）

### DetectionSnapshot

```cpp
struct DetectionSnapshot {
  int64_t source_sequence = -1;
  int64_t source_pts = 0;
  int64_t generated_time_ms = 0;
  std::vector<Detection> detections;
  bool valid() const { return source_sequence >= 0; }
  bool expired(int64_t now_ms, int64_t ttl_ms) const;
};
```

- 推理帧更新 snapshot
- 非推理帧复用最近 snapshot（`!expired()` 时绘框）
- 过期不绘制（避免检测框永久粘附）

## CLI 用法

```bash
export LD_LIBRARY_PATH=/opt/sophon/sophon-ffmpeg_0.8.0/lib:/opt/sophon/libsophon-0.4.9/lib
./build/single_video_infer \
  --input testdata/test.mp4 \
  --output artifacts/stage4/output_single_stream.mp4 \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --device 0 --decoder h264_bm --encoder h264_bm \
  --output-fps 10 --inference-fps 5 --bitrate-kbps 800 --gop 20 \
  --queue-size 1 --conf 0.1 --iou 0.1 --result-ttl-ms 1000 \
  --loop 1
```

响应 `SIGINT`/`SIGTERM` 优雅停止。

## 已知限制

- **CPU 预处理瓶颈**：当前 sws_scale NV12→RGB 640×640 + NCHW 约 22ms/帧，加上 sws_scale 960×544 转换与绘框开销，总处理约 200ms/帧，制约输出帧率至约 5fps（低于 10fps 目标）
- **阶段4.1 优化方向**：BMCV/VPP 硬件预处理（NV12 960×544 → 640×640 → BGR/NCHW 直连设备内存，省去 sws_scale + CPU 拷贝）
- **BM1684 VPU 解码缓冲池**：默认 extra_frame_buffer_num=2 不足以覆盖编码器 11 帧保留，必须经 AVDictionary 设为 ≥20

## BMCV 双路径（阶段4 性能优化）

CLI 选项：
```
--preprocess cpu|bmcv   # 预处理路径，默认 cpu
--draw-mode cpu|bmcv|none  # 绘制路径，默认 cpu
```

| 路径 | 预处理 | 绘制 | fps | 检测正确性 |
|------|--------|------|-----|-----------|
| CPU+CPU（原始） | sws NV12->RGB640 + norm | sws 往返 + CPU 绘框 | ~5 | ✅ 基线 |
| CPU+BMCV（推荐） | sws NV12->RGB640 + norm | BMCV draw_rectangle | ~10 | ✅ 与基线一致 |
| BMCV+BMCV（性能） | BMCV CSC + sws resize + norm | BMCV draw_rectangle | ~10 | ⚠️ IoU 0.94–0.98 |

新增文件：`include/video/bmcv_processor.h`、`src/video/bmcv_processor.cpp`、`tools/video_inference/preprocess_compare.cpp`。

300 秒稳定性测试（推荐配置）：10.003fps、退出码 0、RSS +356KB、P95=150ms 稳定、无残留进程。
