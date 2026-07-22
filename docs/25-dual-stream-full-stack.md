# 阶段4.4 双路全链路架构

## 架构总览

```
A/B RTSP -> h264_bm硬解 -> jitter缓冲 -> 墙钟调度(10fps) -> BMCV VPP预处理
-> BMRuntime推理(5fps) -> 禁区过滤 -> 坐标预测(Sidecar) -> AIS关联(Sidecar)
-> BMCV绘框 -> h264_bm硬编 -> A/B RTMP
```

## 核心组件

### 1. DualStreamApplication
- 管理 two SingleStreamPipeline 实例
- A/B 同时启动，独立 RTSP/RTMP/推理/编码线程
- 全局 SIGINT/SIGTERM 同时停止
- 任一路失败不杀另一路
- 每路独立 metrics、snapshot、source/sink epoch

### 2. 抖动缓冲（Jitter Buffer）
- capture_loop 线程持续读取 RTSP 帧
- 抖动缓冲（默认5帧）吸收突发到达
- decode_loop 按 output_fps 墙钟时间从缓冲取帧
- 解决 B 路突发到达导致输出仅 3fps 的问题（提升至 9fps）

### 3. BmrtDetector 张量预分配
- 构造时分配输入/输出设备 Tensor
- infer() 复用，不再每次 alloc/free
- 减少设备内存碎片和分配开销

### 4. 业务 Sidecar（Python）
- Unix Domain Socket 批量请求/响应
- 坐标预测（sklearn 1.3.2 原生加载真实 A/B 模型，numpy 向量化备选）
- MQTT AIS 订阅 + AIS 缓存
- 视觉-AIS 最近邻匹配
- 超时降级，不阻塞视频路径

### 5. BusinessEnrichmentClient（C++）
- 持久连接 Unix Domain Socket
- 30ms 超时，容量1最新请求
- 降级状态：FULL / COORD_ONLY / DETECTION_ONLY

## BMRuntime 双路策略

### per_stream（当前实现）
- A/B 各加载一份 bmodel
- 独立 BMRuntime 上下文
- 两路推理线程并发
- TPU 内存：~150MB（两份模型 + 张量）

### shared_serialized（后续实现）
- 单份 bmodel + 互斥推理队列
- A->B 公平调度
- 适用于 TPU 内存不足时

## 性能基线

### 单路基线（300秒）
| 路 | 输出fps | 推理fps | 丢帧 | RTMP重连 | 退出码 |
|----|---------|---------|------|----------|--------|
| A  | 7.89    | 3.95    | 0    | 0        | 0      |
| B  | 2.48→9.05 | 1.77→4.53 | 0/6 | 0/1      | 0      |

B 路修复前 2.48fps，修复后（jitter buffer）9.05fps。

### 双路纯视频 T1（60秒）
| 路 | 输出fps | 推理fps | 丢帧 | RTSP重连 | 退出码 |
|----|---------|---------|------|----------|--------|
| A  | 9.62    | 4.82    | 0    | 0        | 0      |
| B  | 7.92    | 4.02    | 0    | 0        | 0      |

### 双路+业务 T2（60秒）
| 路 | 输出fps | 推理fps | 丢帧 | JSONL事件 | 退出码 |
|----|---------|---------|------|-----------|--------|
| A  | 9.52    | 4.75    | 0    | 284       | 0      |
| B  | 7.78    | 3.93    | 0    | 232       | 0      |

业务增强开销 <2%。

## 坐标预测状态

- sklearn 1.3.2 + scipy 1.10.1 已安装（pip3）
- A/B 坐标模型通过 joblib 原生加载成功
- A 路：4特征[x1,y1,x2,y2] -> (lon,lat)，100棵树
- B 路：2特征[x_center,y_center] -> (lon,lat)，100棵树
- 预测耗时：A=0.56ms/次 B=0.62ms/次（满足30ms超时）
- 坐标在杭州湾区域（lon~121.05 lat~30.57）
- 端到端验证：双路60s测试A路280个JSONL事件，坐标与模拟模式不同（确认真实模型生效）；NumpyRandomForest等价验证、真实AIS匹配和长时门禁待完成

## MQTT/AIS 状态

- paho-mqtt 已安装，MQTT 连接成功（地址由环境变量注入）
- 订阅 upAIS/base_2250, upAIS/base_2251
- AIS 6-bit 解码器已实现（支持类型 1/2/3/4/18）
- 测试期间 AIS 缓存为 0（区域内无船或无消息发布）
- 真实 AIS 匹配待有船数据时验证
