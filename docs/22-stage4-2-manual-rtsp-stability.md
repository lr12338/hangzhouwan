# 阶段4.2：单路 RTSP 人工长时稳定性测试指南

> **Agent 不得执行超过 300 秒的 RTSP 测试。** 本文件全部项目由人工执行，最长可达 2 小时。
>
> 前提：已在板端**私下**设置测试 RTSP 环境变量（不在命令行/日志/Git 中出现真实 URL、用户名、密码）：
> ```bash
> export HZW_TEST_RTSP_URL='rtsp://用户名:your_password@测试地址:554/路径'   # 仅在板端交互 shell 设置，勿写入文件
> chmod 700 tools/video_inference
> ```
> 若无真实摄像头，可在另一台机器（x86 等）用 MediaMTX + ffmpeg 把 `testdata/test.mp4` 中继为本地 RTSP 源作为受控测试流。

## 0. 构建与基线（每次测试前）

```bash
cd /home/linaro/hangzhouwan-orign/hangzhouwan
export LD_LIBRARY_PATH=/opt/sophon/sophon-ffmpeg_0.8.0/lib:/opt/sophon/libsophon-0.4.9/lib

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
python3 tests/run_tests.py
(cd build && ctest --output-on-failure && cd ..)
```

## 1. 60 秒实流功能验证（人工，Agent 可执行≤300s 内的同类）

```bash
./build/single_video_infer \
  --source-type rtsp --input-env HZW_TEST_RTSP_URL \
  --output artifacts/stage4_2/rtsp_60s.mp4 \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --device 0 --decoder h264_bm --encoder h264_bm \
  --rtsp-transport tcp --rtsp-stimeout-us 5000000 --rtsp-max-reconnect -1 \
  --source-fps 20 --output-fps 10 --inference-fps 5 --queue-size 1 \
  --preprocess cpu --draw-mode bmcv --max-seconds 60
```

验收：实际解码器 `h264_bm`、编码器 `h264_bm`、输出 fps≈10、推理 fps≈5、队列≤1、退出码 0、ffprobe 通过、无密码泄漏、无残留进程。

## 2. 受控断线重连（人工，≤180s）

```bash
# 1) 正常运行 30s
# 2) 停止测试 RTSP 源 10~20s（断摄像头电源 / 断中继 / 拔网线）
# 3) 恢复测试源
# 4) 确认日志出现 BACKOFF -> 重连成功，再运行 30~60s
# 5) 受控结束（SIGTERM 或 --max-seconds）
```

验收：断流后进入 BACKOFF（无忙循环）、恢复后重新收到帧、bmodel 未重新加载、队列无历史帧、旧检测结果已清除、输出 PTS 单调、输出文件可读、无内存持续增长、退出码 0、无残留。

## 3. 30 分钟连续测试（人工）

```bash
./build/single_video_infer \
  --source-type rtsp --input-env HZW_TEST_RTSP_URL \
  --output artifacts/stage4_2/rtsp_30min.mp4 \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --device 0 --decoder h264_bm --encoder h264_bm \
  --rtsp-transport tcp --rtsp-stimeout-us 5000000 --rtsp-max-reconnect -1 \
  --source-fps 20 --output-fps 10 --inference-fps 5 --queue-size 1 \
  --preprocess cpu --draw-mode bmcv --metrics-interval 30 --max-seconds 1800 \
  2>&1 | tee artifacts/stage4_2/logs/rtsp_30min.log
```

运行中（另开终端）可监控：`bm-smi -noloop`、`free -h`、`ps -o pid,etime,%cpu,%mem,rss,cmd -p <PID>`、`tail -f` 日志。

验收：完整 1800s、退出码 0、ffprobe duration≈1800s、RSS 增长<50MB、TPU 无持续增长、队列≤1、端到端延迟无持续单调增长、无崩溃/Core Dump、无残留进程。

## 4. 2 小时连续测试（人工，最终门禁）

```bash
./build/single_video_infer \
  --source-type rtsp --input-env HZW_TEST_RTSP_URL \
  --output artifacts/stage4_2/rtsp_2hour.mp4 \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --device 0 --decoder h264_bm --encoder h264_bm \
  --rtsp-transport tcp --rtsp-stimeout-us 5000000 --rtsp-max-reconnect -1 \
  --source-fps 20 --output-fps 10 --inference-fps 5 --queue-size 1 \
  --preprocess cpu --draw-mode bmcv --metrics-interval 30 --max-seconds 7200 \
  2>&1 | tee artifacts/stage4_2/logs/rtsp_2hour.log
```

验收：完整 7200s、退出码 0、输出文件正常闭合、ffprobe 通过、RSS 增长<100MB、TPU 无持续增长、输出 fps 稳定、无持续错误、无残留进程。

## 5. 断流 5 分钟后恢复（人工）

运行 2 节命令（`--max-seconds 900`），运行中断流 5 分钟，恢复后确认自动重连并继续输出，退出码 0，输出文件可读，PTS 单调。

## 6. 摄像头重启恢复（人工）

运行中重启测试摄像头；确认程序进入 BACKOFF、摄像头就绪后自动重连、继续输出、退出码 0。

## 7. 网络抖动（人工）

在测试源与板端之间制造丢包/延迟（如 `tc netem`，仅限受控测试网络，不得影响生产网卡）；确认无忙循环、退避合理、恢复后正常、退出码 0。

## 8. SIGTERM 优雅退出（人工）

```bash
./build/single_video_infer --source-type rtsp --input-env HZW_TEST_RTSP_URL \
  --output artifacts/stage4_2/rtsp_sigterm.mp4 \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --decoder h264_bm --encoder h264_bm --rtsp-transport tcp \
  --source-fps 20 --output-fps 10 --inference-fps 5 --queue-size 1 \
  --preprocess cpu --draw-mode bmcv --max-seconds 1800 &
PID=$!; sleep 30; kill -TERM $PID; wait $PID; echo "exit=$?"
```

验收：日志出现"收到信号 15，请求停止"、输出文件正常闭合、ffprobe 通过、退出码 0、无残留进程。

## 9. 输出验证与残留检查（每项测试后）

```bash
ffprobe -v error -show_entries format=duration,bit_rate \
  -show_entries stream=codec_name,width,height,r_frame_rate,nb_frames \
  -of default=noprint_wrappers=1 <输出文件>
pgrep -x single_video_infer || echo "无残留"
python3 tools/redact_secrets.py --scan .        # 提交前
git grep -nEi 'rtsp://[^ ]+:[^ ]+@' || true     # 提交前（应无真实凭据）
```

## 10. 状态声明

- 板端已验证：RTSP 连接/超时/中断/脱敏路径（受控无效地址）、全部单元测试、20s 本地文件回归。
- **待人工执行**：实流解码、中途断线重连、30min/2h 长时门禁。
- 在人工提供完整实流运行日志前，不得声明 RTSP 长时稳定性已通过。
