# 阶段4.4 坐标预测/AIS/MQTT 架构

## 组件架构

```
C++ 视频热路径                         Python 业务 Sidecar
┌─────────────────────┐               ┌─────────────────────────┐
│ SingleStreamPipeline │               │ business_sidecar.py      │
│  ├─ 检测 + 禁区过滤  │               │  ├─ CoordinatePredictor  │
│  ├─ BusinessEnrichmentClient ────────>│  │   ├─ A: joblib/模拟  │
│  │   (Unix Domain Socket)│           │  │   └─ B: joblib/模拟  │
│  ├─ BMCV 绘框        │<──────────────│  ├─ AisStore (线程安全)  │
│  └─ JSONL 输出       │  响应(坐标+匹配)│  ├─ MqttAisSubscriber   │
└─────────────────────┘               │  │   └─ paho-mqtt        │
                                      │  ├─ AIS 6-bit 解码器     │
                                      │  └─ 最近邻匹配器         │
                                      └─────────────────────────┘
```

## 坐标预测

### A 路（北下）
- 模型：`weights/0121_random_forest_model.pkl`
- 输入特征：`[x1, y1, x2, y2]`（完整检测框坐标）
- 输出：`(longitude, latitude)`

### B 路（南上）
- 模型：`weights/beishang_x-l.pkl`
- 输入特征：`[x_center, y_center]`（检测框中心点）
- 输出：`(longitude, latitude)`

### 状态
- sklearn 1.3.2 + scipy 1.10.1 已安装（pip3 install scikit-learn==1.3.2）
- A/B 模型通过 joblib.load 原生加载成功
- 预测耗时：A=0.56ms/次 B=0.62ms/次
- 坐标在杭州湾区域（lon~121.05 lat~30.57）
- 端到端验证通过：真实坐标与模拟坐标不同（dlon=0.027 dlat=0.016）
- 备选：numpy 向量化实现（无需 sklearn 运行时，0.56ms/次）

## MQTT AIS

### 配置
- Broker: 由环境变量 AIS_MQTT_HOST:AIS_MQTT_PORT 注入
- Client ID: 由环境变量 AIS_MQTT_CLIENT_ID 注入
- Topics: `upAIS/base_2250`, `upAIS/base_2251`

### AIS 解码
- 自实现 6-bit ASCII 解码器（无需 pyais 依赖）
- 支持消息类型：1/2/3（Class A 位置）、4（基站）、18（Class B 位置）
- 提取字段：MMSI、经度、纬度、速度、航向

### AIS 缓存（AisStore）
- 线程安全（threading.Lock）
- 最大容量 500 艘船
- 30 秒过期清理
- 统计：消息数、解析成功数、失败数、缓存船数
- **已验证**：MQTT发布测试消息 -> 解析成功 -> 缓存更新 -> 匹配成功

## 视觉-AIS 匹配

### 处理顺序
```
YOLO 原始输出 -> NMS -> 禁区过滤 -> 坐标预测 -> AIS 最近邻匹配 -> 业务结果 -> 绘框/元数据
```

### 匹配规则
- 最大匹配距离：0.5km（可配置）
- 每帧同一 MMSI 最多匹配一个视觉框
- 一个视觉框最多匹配一个 AIS
- 全局距离排序匹配（不依赖遍历顺序）
- haversine 距离计算

## 降级策略

| 状态 | 条件 | 行为 |
|------|------|------|
| FULL | 坐标 + AIS 匹配成功 | 完整业务输出 |
| COORD_ONLY | 坐标成功，AIS 不可用 | 仅坐标，无匹配 |
| DETECTION_ONLY | Sidecar 超时或不可用 | 仅检测，无业务 |

- 超时：30ms（可配置）
- Sidecar 不可用时不停止 RTSP/推理/编码/RTMP
- 降级和恢复次数有计数

## JSONL 输出

每路输出轻量业务 JSONL：
```
artifacts/internal-development/stream_A_events.jsonl
artifacts/internal-development/stream_B_events.jsonl
```

格式：
```json
{"sid":"A","seq":123,"n":2,{"lon":121.03,"lat":30.55,"v":1,"m":0,"mmsi":""},{...}}
```

- `v`: 坐标有效 (0/1)
- `m`: AIS 匹配 (0/1)
- 文件已 gitignore，不提交仓库
