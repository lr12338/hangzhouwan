# 阶段4.3 人工长时测试指南

## 前提

A、B 两路单路 300 秒短测均已通过。本指南提供 30 分钟和 2 小时长时测试的人工命令，不由 Agent 执行。

## 环境准备

```bash
export LD_LIBRARY_PATH=/opt/sophon/sophon-ffmpeg_0.8.0/lib:/opt/sophon/libsophon-0.4.9/lib
cd /home/linaro/hangzhouwan
```

## A路 30 分钟测试

```bash
./build/single_video_infer \
  --source-type rtsp \
  --input 'rtsp://admin:***@112.16.184.176:48554/streaming/Channels/801' \
  --sink-type rtmp \
  --output 'rtmp://hangzhouwanpush.hifleet.com:1935/HangZhouBridge/HangZhouBridgeNorth8_1' \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --device 0 --decoder h264_bm --encoder h264_bm \
  --rtsp-transport tcp --rtsp-stimeout-us 5000000 \
  --preprocess bmcv --draw-mode bmcv \
  --output-fps 10 --inference-fps 5 --bitrate-kbps 800 --gop 20 \
  --queue-size 1 --conf 0.1 --iou 0.1 --max-seconds 1800
```

## A路 2 小时测试

```bash
./build/single_video_infer \
  --source-type rtsp \
  --input 'rtsp://admin:***@112.16.184.176:48554/streaming/Channels/801' \
  --sink-type rtmp \
  --output 'rtmp://hangzhouwanpush.hifleet.com:1935/HangZhouBridge/HangZhouBridgeNorth8_1' \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --device 0 --decoder h264_bm --encoder h264_bm \
  --rtsp-transport tcp --rtsp-stimeout-us 5000000 \
  --preprocess bmcv --draw-mode bmcv \
  --output-fps 10 --inference-fps 5 --bitrate-kbps 800 --gop 20 \
  --queue-size 1 --conf 0.1 --iou 0.1 --max-seconds 7200
```

## B路对应测试

将 `--input` 替换为 B 路 RTSP 地址，`--output` 替换为 B 路 RTMP 地址，`Channels/801` 改为 `Channels/301`，`HangZhouBridgeNorth8_1` 改为 `HangZhouBridgeNorth3_1`。

## 验收标准

- 退出码 0
- 无 BMRuntime、VPU 或 FFmpeg 持续错误
- RTMP 重连次数合理（网络波动可接受少量重连）
- 输出帧数与时长相符（10fps x 秒数 ±10%）
- 推理次数与时长相符（5fps x 秒数 ±10%）
- 队列不超过 1，丢帧率 < 5%
- 接收端画面持续可见，包含检测框
- 无残留进程

## 注意事项

- 真实地址请从 `/data/hangzhouwan/config/internal-development.yaml` 获取
- 不得同时运行 A 和 B
- 长时测试期间观察 CPU、RSS 和 TPU 占用
- 如出现性能不足，可调整 `--output-fps`、`--inference-fps` 或 `--bitrate-kbps`
