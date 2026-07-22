# 双路并发视频推理工具

## 概述

A/B 双路并发视频推理，各自独立 RTSP/RTMP/推理/编码。全局 SIGINT/SIGTERM 同时停止两路。
任一路失败不杀另一路。每路拥有独立 metrics、snapshot 和 source/sink epoch。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

## 用法

```bash
# 设置环境变量
export STREAM_A_INPUT_URL='rtsp://...'
export STREAM_A_OUTPUT_URL='rtmp://...'
export STREAM_B_INPUT_URL='rtsp://...'
export STREAM_B_OUTPUT_URL='rtmp://...'

# 启动业务 Sidecar（坐标预测 + MQTT AIS）
python3 tools/business/business_sidecar.py &

# 运行双路（60秒）
./build/dual_stream_app --max-seconds 60 --jitter-buffer-size 5 --enable-business

# 纯视频（无业务）
./build/dual_stream_app --max-seconds 60 --jitter-buffer-size 5

# 降帧率配置
./build/dual_stream_app --max-seconds 60 --output-fps 8 --inference-fps 4
```

## 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| --max-seconds | 0 | 最长运行秒数（0=不限） |
| --metrics-interval | 10 | 指标输出间隔秒 |
| --detector-mode | per_stream | per_stream \| shared_serialized |
| --jitter-buffer-size | 5 | 抖动缓冲帧数（吸收RTSP突发） |
| --output-fps | 10 | 输出帧率 |
| --inference-fps | 5 | 推理帧率 |
| --no-region-filter | - | 禁用禁区过滤 |
| --enable-business | - | 启用业务增强（坐标+AIS） |
| --business-socket | /tmp/hangzhouwan-business.sock | Sidecar Unix Socket |
| --streams | A,B | 指定启动的流 |

## 人工长时测试

```bash
./tools/dual_stream/dual_full_stack_stability.sh 30   # 30分钟
./tools/dual_stream/dual_full_stack_stability.sh 120  # 2小时
```

## 架构

```
DualStreamApplication
├── Stream A / SingleStreamPipeline (capture → jitter → schedule → decode → infer → draw → encode → RTMP)
├── Stream B / SingleStreamPipeline (同上)
├── BusinessEnrichmentClient A → Sidecar (坐标预测 + AIS匹配)
├── BusinessEnrichmentClient B → Sidecar
├── global metrics collector
└── global signal/shutdown controller
```

## 禁区配置

A 路和 B 路的禁区已内置（参考分辨率 2560×1440）：
- A: 矩形3个 + 多边形1个（北岸摄像头）
- B: 矩形1个 + 多边形1个（南岸摄像头）
