# Bridge Capture 抓拍服务运维指南

AIS 趋近预警驱动的南北通航孔船舶抓拍服务。独立 executable `bridge_capture_app`，
复用 `hzw_inf`（SophonVideoSource/BmrtDetector/BmcvProcessor/YOLO/JPEG），不复制源码，
不影响现有 A/B 双路生产视频业务。

## 架构

```
hangzhouwan-bridge (Python)
  AIS -> ApproachDetector -> ApproachEvent
    ↓ UDS arm 命令
bridge_capture_app (C++, 独立进程)
  IDLE -> 开视频 -> 3~5FPS YOLO推理 -> 最佳帧 -> 原始帧BMCV crop -> JPEG
  ↓
/data/hangzhouwan/bridge/captures/ready/*.jpg
```

- bridge 只回答"哪条船、哪个通航孔、什么时候到"。
- capture 负责视频/推理/裁剪/JPEG；IDLE 时不连视频、不占 VPU、不推理。
- north/south 共用 1×BmrtDetector；同 bridge 同时只允许一个 active session。

## 服务职责

| 服务 | 进程 | 职责 |
|---|---|---|
| `hangzhouwan-bridge.service` | Python | AIS/轨迹/Approach/CaptureArm 调度 |
| `hangzhouwan-bridge-capture.service` | `bridge_capture_app` | 视频抓拍 |
| `hangzhouwan-video.service` | `dual_stream_app` | **生产 A/B，不受影响** |

三者独立；capture 崩溃不影响 bridge AIS 检测，也不影响 video A/B。

## 配置

### 环境变量 (`/etc/hangzhouwan/bridge-capture.env`)

真实 RTSP URL 仅放此文件，不入 Git（见 `config/bridge-capture.env.example`）：

```bash
CAPTURE_NORTH_URL=rtsp://user:pass@camera-north:554/Streaming/Channels/101
CAPTURE_SOUTH_URL=rtsp://user:pass@camera-south:554/Streaming/Channels/101
```

### bridge 侧 (`/etc/hangzhouwan/bridge.yaml`)

```yaml
capture:
  enabled: true                       # bridge_capture_app 部署后开启
  socket_path: /run/hangzhouwan/bridge-capture.sock
  timeout_sec: 3
```

## 启动

```bash
# 1. 部署 env
sudo cp config/bridge-capture.env.example /etc/hangzhouwan/bridge-capture.env
sudoedit /etc/hangzhouwan/bridge-capture.env   # 填真实 URL

# 2. 安装服务
sudo cp deploy/systemd/hangzhouwan-bridge-capture.service /etc/systemd/system/
sudo systemctl daemon-reload

# 3. 启动（先确认 bridge_capture_app 在 /opt/hangzhouwan/current/bin/）
sudo systemctl start hangzhouwan-bridge-capture
sudo systemctl enable hangzhouwan-bridge-capture

# 4. bridge 侧开启 capture trigger
#    编辑 /etc/hangzhouwan/bridge.yaml: capture.enabled: true
sudo systemctl restart hangzhouwan-bridge
```

## 状态

```bash
# capture 健康检查（UDS）
python3 -c "
import socket,struct,json
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
s.connect('/run/hangzhouwan/bridge-capture.sock')
req=json.dumps({'cmd':'health'}).encode()
s.sendall(struct.pack('!I',len(req))+req)
l=struct.unpack('!I',s.recv(4))[0]
print(json.loads(s.recv(l)))
"

# bridge 侧抓拍统计
bridge-ctl capture
bridge-ctl health   # 含 capture 字段
```

## 日志

```bash
journalctl -u hangzhouwan-bridge-capture -f
# 关键行：
#   信息 | capture | 模型加载成功 net=...
#   信息 | capture | UDS server 监听 /run/hangzhouwan/bridge-capture.sock
#   信息 | capture | rtsp://...:***@... 视频打开 2560x1440   (脱敏)
```

## 手工 ARM 测试

不经 UDS，直接 CLI 测试单路抓拍（P2 板端验证）：

```bash
export CAPTURE_NORTH_URL='rtsp://user:pass@camera-north:554/...'
/opt/hangzhouwan/current/bin/bridge_capture_app \
  --bmodel /opt/hangzhouwan/current/weights/yolov7.bmodel \
  --device 0 \
  --capture-dir /data/hangzhouwan/bridge/captures \
  --north-url-env CAPTURE_NORTH_URL \
  --cli-arm north --mmsi 414402810 --direction upstream --timeout 60
```

观察：视频打开 -> 推理 -> 找船 -> JPEG 落盘 -> 视频关闭 -> VPU inflight 归零。

## 抓拍目录

```
/data/hangzhouwan/bridge/captures/
├── pending/    # 临时写入（.tmp）
├── ready/      # 原子 rename 后的最终 JPEG + metadata
├── failed/
└── diagnostics/
```

文件名：`bridge_mmsi_YYYYmmddHHMMSS_session8.jpg`

## VPU 生命周期（硬门禁）

CaptureSession 关闭遵循现有安全模式（防止 VPU heap 泄漏）：
1. `request_stop()` 停止产生新帧
2. `wait_avframes_drained(drain_timeout)` 等待在途帧归零
3. `close()` 关闭解码器

IDLE 时不开视频、不占 VPU decoder、不运行推理循环。

## 资源隔离

- OOM 优先级：video=-500（生产），capture=200（压力时优先杀 capture）
- 独立 MemoryHigh=384M / MemoryMax=512M
- capture 异常退出 **不** 导致 video/bridge restart（`PartOf` 而非 `Requires`）
- 与 video 共享同一 BM1684 设备/TPU；南北同时 active 时每路 3 FPS

## 常见故障

| 现象 | 排查 |
|---|---|
| `arm 未送达 ... capture socket 不可用` | `bridge_capture_app` 未运行；`systemctl status hangzhouwan-bridge-capture` |
| `BMCV crop: 裁剪区域过小` | bbox 太小（<32px）；正常情况船目标足够大 |
| `RESOURCE_FATAL` | VPU/TPU 资源耗尽；检查 `dmesg`，重启 capture 服务 |
| 抓拍 0 张但 ARM accepted | 检查 ROI 配置/视频是否有船/推理 conf 阈值；查 diagnostics |
| VPU heap 增长 | 确认 inflight 归零日志；抓拍失败可接受，VPU 泄漏不可接受 |

## 回滚

```bash
sudo systemctl stop hangzhouwan-bridge-capture
sudo systemctl disable hangzhouwan-bridge-capture
# bridge 侧关闭 trigger
sudo sed -i 's/enabled: true/enabled: false/' /etc/hangzhouwan/bridge.yaml
sudo systemctl restart hangzhouwan-bridge
```

回滚 capture 不影响 video A/B 与 bridge AIS 检测。

## 验证状态

### LOCAL HARDWARE VALIDATION（PASS）

实际命令与结果（BM1684 + bmodel `artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel` + testdata/test.mp4）：

```bash
# 单元 / 链路回归
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
# 21/21 passed（含新增 jpeg_encode）

cd /home/linaro/hangzhouwan
python3 -m pytest -q
# 207 passed

# 本地硬件 E2E（用 test.mp4 代替生产 RTSP，conf=0.05 强制低阈值出图）
build/bridge_capture_app \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --capture-dir /tmp/hzw_capture_run2 \
  --cli-file testdata/test.mp4 \
  --cli-arm north --mmsi 999999999 --direction upstream \
  --timeout 25 --conf 0.05 --iou 0.3 --fps 10
# 完整链路：VPU h264_bm 解码 -> BMCV 预处理 -> YOLO 推理 -> BMCV crop -> JPEG
# 结果：ready/north_999999999_19700124000751_cli-1786.jpg, 3198 字节, 84x64 RGB
```

资源生命周期（VPU decoder `close()` 干净退出，inflight 归零，SIGTERM 干净退出）此前已验证，
本轮重新跑 `ctest` 全绿即等同回归通过。

bridge 侧同步验证：

```bash
cd /data/hangzhouwan-bridge
python3 -m pytest tests/ -q
# 68 passed
```

### REAL CAMERA VALIDATION（2026-08-12 板端 PASS）

- `CAPTURE_NORTH_URL`：`rtsp://admin:***@112.16.184.176:48554/streaming/Channels/801`
  （与 hangzhouwan A 路生产流同源；写于 `/etc/hangzhouwan/bridge-capture.env`）
- `CAPTURE_SOUTH_URL`：**未配置**

UDS `arm` 命令接受 → RTSP 2560×1440@25fps 连接成功 → YOLOv7 推理
检出 bbox 47x11 px → `crop_to_rgb(auto_upscale=true)` 沿检测框居中
扩展源 crop 至 32x32 → 候选窗口 1500ms → 候选结束 → BMCV crop + JPEG
encode 成功。

`REAL CAMERA VALIDATION: PASS`

JPEG 落盘示例（手动 ARM 两次，文件大小不同因内容不同）：
- `/data/hangzhouwan/bridge/captures/ready/north_414402810_19700124013713_manual-n.jpg`
  964 B, 32x32, JFIF 1.01
- `/data/hangzhouwan/bridge/captures/ready/north_414402810_19700124014053_manual-n.jpg`
  787 B, 32x32, JFIF 1.01

auto_upscale 是 `BmcvProcessor::crop_to_rgb` 的新参数：
- 旧版（auto_upscale=false）保持原 32px 硬门禁语义，调用方不变。
- capture 引擎走 auto_upscale=true，远端小船也能产出抓拍（保住 ship
  位置上下文，向四周多取几像素），不下采样原图。

AIS → Capture 自动 trigger 已启用：
- `/etc/hangzhouwan/bridge.yaml` 加 `capture: {enabled: true, ...}`
- `bridge-ctl health` 可见 `capture trigger enabled -> /run/hangzhouwan/bridge-capture.sock`
- 桥区船舶自然过桥观察进行中（已 8+ 分钟无 APPROACH；0 events_today 不是
  系统问题，是 Hangzhou Bay Bridge 真实航运规律）。

### KNOWN LIMITATIONS

- **JPEG 链接坑**：板端 `libbmcv.so` 内嵌了 libjpeg v62 符号表，
  若 `libjpeg.so` 不在 `DT_NEEDED` 且排在 `libbmcv.so` 之前，运行时会被
  强制使用 bmcv 内嵌的旧版 jpeg（`JPEG library version: library is 62, caller expects 80`），
  导致 `encode_jpeg()` 静默失败。
  修复：所有使用 `image_io/jpeg_io.h` 的 target 在 `target_link_libraries` 中
  把 `JPEG::JPEG` 显式列在 `hzw_inf` 之前。`test_jpeg_encode` 即用于防回归。
- 单 bmodel 南北复用：`1 × BmrtDetector`，南北同时 active 时单路推理
  FPS 降至 `inference_fps_shared`（默认 3），总 TPU 占用不翻倍。
- `cli-file` 模式只用于板端验证，**禁止**部署到生产 systemd 单元。
