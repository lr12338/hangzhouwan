# 杭州湾双路检测系统 · 生产运维手册

> 本文档为 BM1684 板端生产部署的主要操作参考，可直接复制执行。
> 适用版本：2026-07 单机工业生产加固 Release。
> **当前状态：已替代 Windows 旧服务，双路推流正式地址，生产运行中。**

---

## 1. 系统组成

```
hangzhouwan.target
├── hangzhouwan-business.service   (业务增强 Sidecar)
├── hangzhouwan-video.service      (双路视频管线)
└── hangzhouwan-supervisor.service (30 秒健康监督与有限恢复)

hangzhouwan-maintenance-restart.timer
└── 每天 03:30 顺序重启 Business 和 Video，并执行就绪检查
```

- **Business**：Python sidecar，负责坐标预测（sklearn）、MQTT AIS 订阅/缓存、视觉-AIS 匹配、JSONL 事件输出。
- **Video**：C++ `dual_stream_app`，负责双路 RTSP 接入、h264_bm 硬解、BMCV 预处理、BMRuntime 推理、禁区过滤、融合快照绘制、h264_bm 硬编、RTMP 推流。
- 两者通过 Unix Domain Socket 通信。
- Business 异常时 Video 自动降级为 `DETECTION_ONLY`（仅检测，无坐标/AIS），Business 恢复后自动恢复业务融合。

### 依赖关系

- Video `Wants=hangzhouwan-business.service`（启动 Video 时一并拉起 Business，但 Business 故障不连带停止 Video）。
- Video `After=hangzhouwan-business.service`（保证 Business 先启动）。
- Video `ExecStartPre=hzwctl wait-business --timeout 30`（等 Business readiness 通过后才启动 Video 主程序）。
- 共享运行目录 `/run/hangzhouwan` 由 `tmpfiles.d` 统一管理，不随任一服务停止而删除。
- Video 为 10 分钟最多 3 次、间隔 60 秒；Business 为 5 分钟最多 5 次、
  间隔 15 秒。限流后由监督器结合上游、磁盘和冷却策略处理。
- 定时维护使用 `Persistent=true`；补执行时若开机或 Video 运行不足
  30 分钟会跳过，避免启动后立即二次重启。

---

## 2. 当前生产配置

### 推流地址（正式地址，已替代 Windows 旧服务）

| 流 | RTSP 输入 | RTMP 输出 |
|---|---|---|
| A 路（南下） | `rtsp://admin:***@112.16.184.176:48554/streaming/Channels/801` | `rtmp://hangzhouwanpush.hifleet.com:1935/HangZhouBridge/HangZhouBridgeNorth8_1` |
| B 路（北上） | `rtsp://admin:***@112.16.184.176:48554/streaming/Channels/301` | `rtmp://hangzhouwanpush.hifleet.com:1935/HangZhouBridge/HangZhouBridgeNorth3_1` |

> RTSP/RTMP 凭据通过 `/etc/hangzhouwan/video.env` 注入，不写入 application.yaml。

### AIS 订阅

- MQTT Broker：`iot.hifleet.com:1883`
- 订阅主题：`upAIS/#`（通配符，接收所有 AIS 基站数据）
- 原 `upAIS/base_2250`、`upAIS/base_2251` 基站已离线，改用通配符订阅
- `upAIS/dtu_shui_yu` 主题发送 JSON 格式 AIS 数据（非 AIVDM），解码器已增加 JSON 解析支持

### 已知限制

- B 路 `Channels/301` 摄像头偶尔返回 400 Bad Request（摄像头端固件问题），重连后可恢复
- AIS 匹配半径 500m，仅匹配摄像头视野内的船舶；远处 AIS 船舶不误匹配
- `kern.log` 因 VPU clock 开关日志刷屏，已通过 journald 限制 + rsyslog 轮转控制

---

## 3. 目录结构

| 路径 | 说明 |
|---|---|
| `/opt/hangzhouwan/current` | 当前激活 Release 软链接 |
| `/opt/hangzhouwan/previous` | 上一版本 Release 软链接（回滚目标） |
| `/data/hangzhouwan/releases/<version>-<commit>/` | 不可变 Release 制品目录 |
| `/etc/hangzhouwan/application.yaml` | 唯一权威配置（权限 640 root:hangzhouwan） |
| `/etc/hangzhouwan/business.env` | Business 环境变量（MQTT 凭据等，权限 640） |
| `/etc/hangzhouwan/video.env` | Video 环境变量（RTSP/RTMP URL 等，权限 640） |
| `/etc/tmpfiles.d/hangzhouwan.conf` | 共享运行目录 tmpfiles.d 配置 |
| `/etc/systemd/journald.conf.d/50-hangzhouwan-limits.conf` | journald 总量限制（50M） |
| `/run/hangzhouwan/business.sock` | Business Sidecar Unix Socket |
| `/run/hangzhouwan/video-health.sock` | Video 结构化健康接口 Socket |
| `/data/hangzhouwan/events/` | A/B 事件 JSONL、gzip 轮转和导入归档 |
| `/data/hangzhouwan/monitor/` | 本地告警、MQTT 离线队列、监督器状态和诊断 |

Release 目录结构：
```
/data/hangzhouwan/releases/<version>-<commit>/
├── bin/dual_stream_app       # C++ 双路推理主程序
├── bin/hzwctl                # 运维工具
├── venv/                     # 固定版本、自包含且不引用 /home 的 Python 环境
├── models/                   # bmodel + 坐标模型
├── services/                 # Business sidecar Python 代码
├── systemd/                  # systemd 单元文件
├── tmpfiles.d/               # tmpfiles.d 配置
├── config/                   # 示例配置
├── VERSION                   # 版本信息
├── manifest.json             # 文件清单 + SHA256
└── sha256sum.txt             # 完整性校验
```

Release 必须为 `root:root` 且组/其他用户不可写；安装和激活前使用
`verify_release.sh` 校验完整 SHA256。

---

## 4. 启动服务

### 启动全部（通过 target）

```bash
sudo systemctl start hangzhouwan.target
```

### 推荐分步启动

```bash
# 1. 先启动 Business
sudo systemctl start hangzhouwan-business.service
sudo /opt/hangzhouwan/current/bin/hzwctl wait-business --timeout 30

# 2. 再启动 Video
sudo systemctl start hangzhouwan-video.service
sudo /opt/hangzhouwan/current/bin/hzwctl wait-health --timeout 180
```

- **何时用 target**：常规启动，两个服务都需要运行。
- **何时分步启动**：调试 Business 单独问题，或验证 readiness 门禁。
- **readiness 失败**：检查 `journalctl -u hangzhouwan-business.service` 或 `journalctl -u hangzhouwan-video.service`，常见原因为 MQTT 不可达、模型加载失败、TPU 设备不可用。

---

## 5. 停止服务

```bash
# 停止全部
sudo systemctl stop hangzhouwan.target

# 单独停止
sudo systemctl stop hangzhouwan-video.service
sudo systemctl stop hangzhouwan-business.service
```

### 5.1 每日维护重启

默认每天北京时间 03:30 执行受控维护，顺序为：

1. 获取维护锁，保存诊断，检查 `/data`、网络和上游端口；
2. 清除限流后重启 Business，并验证 Sidecar；
3. 重启 Video，连续三次验证 A/B 真实输出和推理；
4. 180 秒内未达严格健康则保存诊断并进入持久恢复状态；至少一路仍有
   RTSP/RTMP、输出和推理时保持降级运行，只有全部不可用或资源致命时才全停；
5. 恢复 timer 每 30 分钟检查一次。降级可用时只告警和复查，Business
   单独失败时只重启 Business；完全不可用时才重建 Business/Video。

安装并启用：

```bash
sudo bash tools/release/install_release.sh /opt/hangzhouwan/current
sudo systemctl enable --now hangzhouwan-maintenance-restart.timer
sudo systemctl enable --now hangzhouwan-maintenance-recovery.timer
systemctl list-timers 'hangzhouwan-maintenance-*'
```

升级该版本前，必须将 `/etc/hangzhouwan/application.yaml` 的
`health.stream_healthy_fps` 更新为 `9`，并补齐示例中的根盘、恢复间隔和
诊断保留配置；生产预检会拒绝仍使用 7fps 健康线的配置。
`hzwctl wait-health` 始终用于严格验收；故障恢复现场可使用
`hzwctl wait-operational` 判断至少一路数据面是否仍可服务。

修改执行时间后，执行 `sudo systemctl daemon-reload && sudo systemctl restart
hangzhouwan-maintenance-restart.timer`。临时手动触发可运行：

```bash
sudo systemctl start hangzhouwan-maintenance-restart.service
```

关闭定时重启：

```bash
sudo systemctl disable --now hangzhouwan-maintenance-restart.timer
```

检查残留进程：

```bash
pgrep -af 'dual_stream_app|business_enrichment' || true
```

---

## 6. 重启服务

```bash
# 全部重启
sudo systemctl restart hangzhouwan.target

# 只重启 Video（Business 不受影响，business.sock 不丢失）
sudo systemctl restart hangzhouwan-video.service

# 只重启 Business（Video 自动降级，Business 恢复后自动恢复融合）
sudo systemctl restart hangzhouwan-business.service
```

- **全部重启**：同时重启 Business 和 Video，短暂中断推流。
- **只重启 Video**：Business 继续运行，Video 恢复后自动重连 Business Socket。
- **只重启 Business**：Video 继续推流（降级为 DETECTION_ONLY），Business 恢复后 Video 自动恢复融合。

### 6.1 故障恢复分级（先只读排查，保存证据后再恢复）

> 未经现场授权，不得激活候选 Release、修改生产配置、回滚、整机重启、停止 Business、故障注入、连续反复重启 Video、删除 Release/日志/证据。

| 优先级 | 操作 | 适用场景 | 边界 |
|---|---|---|---|
| 1 | 保存日志 + VPU Heap 快照 | 任何故障第一时间 | 只读，安全；`bm-smi -noloop` + journalctl 存盘（见 8.1） |
| 2 | Video-only 重启一次 | 单路/双路推流中断、VPU 分配失败但 heap 未耗尽 | `sudo systemctl restart hangzhouwan-video.service`；Business 不受影响 |
| 3 | 停止反复重启 | Video-only 重启后快速复发（<10min 再现 B 路故障） | 连续重启会加剧 VPU 碎片化；改用单路降级或整机重启 |
| 4 | 单路降级 | B 路持续故障、A 路正常 | 配置 `run_b: false` 仅保留 A 路（或反之），避免故障路拖累整机 |
| 5 | 受控整机重启 | VPU heap 耗尽、双路均不可用、资源致命反复 | `sudo reboot`；重启后 heap 由内核全部回收；验证 G0 启动门禁 |

### 6.2 单路降级策略

当单路（如 B）RTSP 持续不可恢复时，可临时降级为单路运行，保证 A 路推流不中断：

- 修复版 A/B 隔离：B 路资源致命/断流不会破坏 A 路已建立的 decoder（独立 epoch/队列/线程）。
- 降级操作：编辑 `/etc/hangzhouwan/application.yaml`，将故障路 `run: false`，重启 Video。
- 恢复：故障路网络恢复后改回 `run: true` 重启；hzwctl `状态` 应恢复 HEALTHY。
- 健康可见性：hzwctl `状态: DEGRADED`、`降级状态: stream_down`、`原因: B路不可用` 会明确标识，不再出现"降级状态 none"假健康。

---

## 7. 查看系统状态

```bash
# 系统状态汇总（优先读 Video 健康接口）
sudo /opt/hangzhouwan/current/bin/hzwctl status

# 健康状态
sudo /opt/hangzhouwan/current/bin/hzwctl health --json

# 版本信息
sudo /opt/hangzhouwan/current/bin/hzwctl version

# 运行时预检
sudo /opt/hangzhouwan/current/bin/hzwctl preflight \
  --release /opt/hangzhouwan/current \
  --config /etc/hangzhouwan/application.yaml \
  --runtime
```

`status` 输出字段说明：
- **当前 Release**：版本号和路径
- **Business 服务**：active/inactive/failed + coordinate_mode + MQTT 状态 + AIS 缓存
- **Video 服务**：active/inactive/failed + 整体状态 + 各路状态 + A/B RTSP/RTMP 状态 + output/inference fps + 重连次数 + 最近帧 + 队列长度 + e2e P95 + 降级状态 + uptime + RSS + TPU

整体状态（`状态`，最重要，醒目展示，避免"降级状态 none"假健康）：

| status | 含义 | 触发条件 |
|---|---|---|
| `HEALTHY` | 双路正常 | A/B 均 RTSP+RTMP 连接、output_fps≥5、inference_fps>0、无资源致命、无重连风暴 |
| `DEGRADED` | 至少一路降级或业务降级 | 单路 RTSP/RTMP 断开、output/inference fps 归零、重连风暴(单窗口>3)、业务仅检测 |
| `FAILED` | 不可用 | 任一路资源致命(VPU/gmem ENOMEM)、或 A/B 双路均不可用 |

单路状态（`流 A/B: [LEVEL]`）：`HEALTHY` / `DEGRADED` / `FAILED`，判定与整体一致但按单路独立计算（B 路故障不拖累 A 路判定）。

`原因`（health_reason）：降级/失败的人类可读原因（如"B路不可用"、"设备资源致命(VPU/gmem)"），便于直接定位。

`降级状态`（degradation）：`none` / `stream_degraded` / `stream_down` / `detection_only` / `resource_fatal`，标识降级类别。

`health` 命令退出码：`0`=HEALTHY、`1`=DEGRADED、`2`=FAILED（可用于监控告警，DEGRADED 即告警）。

---

## 8. 查看实时日志

```bash
# Business 日志
journalctl -u hangzhouwan-business.service -f

# Video 日志
journalctl -u hangzhouwan-video.service -f

# 全部日志
journalctl -u hangzhouwan-business.service -u hangzhouwan-video.service -f
```

最近日志：

```bash
journalctl -u hangzhouwan-video.service -n 200 --no-pager
journalctl -u hangzhouwan-business.service -n 200 --no-pager
```

按时间查看：

```bash
journalctl -u hangzhouwan-video.service \
  --since "2026-07-22 10:00:00" \
  --until "2026-07-22 11:00:00" \
  --no-pager
```

只看错误：

```bash
journalctl -u hangzhouwan-video.service -p warning..alert --since today --no-pager
```

查询关键字：

```bash
journalctl -u hangzhouwan-video.service --since today --no-pager \
  | grep -E 'RTSP|RTMP|重连|错误|失败|DEGRADED'

journalctl -u hangzhouwan-business.service --since today --no-pager \
  | grep -E 'MQTT|AIS|模型|错误|失败'
```

编码前画面诊断（维护窗口使用）：在 Video 的环境文件中临时设置
`HZW_PREENCODE_DUMP_DIR=/data/hangzhouwan/monitor` 并重启 Video。每路只导出
第一张缩放后的主机 YUV420P 帧，不持续写盘。检查完成后删除该环境变量。

```bash
ffmpeg -f rawvideo -pixel_format yuv420p -video_size 1280x720 \
  -i /data/hangzhouwan/monitor/preencode_A_1280x720.yuv \
  -frames:v 1 /data/hangzhouwan/monitor/preencode_A_1280x720.png
```

### 8.1 日志查询规模限制（生产排障必须遵守）

生产日志可能含大量重复字符和错误风暴。**禁止**直接执行 `journalctl --no-pager`、`dmesg`、`cat 超大日志`。每次查询默认满足：

- 时间窗口 ≤ 5～10 分钟；原始输出 ≤ 200 行；展示关键日志 ≤ 80 行；单次文本 ≤ 30 KB。
- **先统计，再提取首次/末次/代表样本**；输出仍过大则立即缩小时间范围或关键词，不得完整打印。

按时间窗口（先统计错误类型，再取样本）：

```bash
# 1) 统计错误类型与次数
video_pid=$(systemctl show hangzhouwan-video.service -p MainPID --value)
journalctl -u hangzhouwan-video.service "_PID=${video_pid}" \
  --since "-10 min" --no-pager -o cat \
  | grep -Eo 'bm_alloc_gmem failed|AllocateDecFrameBuffer|BMVidDecSeqInitW5 failed|invalid free|VPU_DecOpen failed|DEVICE_RESOURCE_FATAL|RTSP_RECONNECT|RTMP重连' \
  | sort | uniq -c | sort -rn

# 2) 围绕故障时间取代表样本（≤200 行）
journalctl -u hangzhouwan-video.service \
  --since "2026-07-23 17:32:30" --until "2026-07-23 17:34:30" \
  --no-pager -o short-iso -n 200

# 3) 限定当前 PID
journalctl -u hangzhouwan-video.service "_PID=${video_pid}" \
  --since "-10 min" --no-pager -o short-iso -n 200
```

保存完整日志但只展示摘要（避免占满上下文）：

```bash
# 存盘完整快照（供事后审计）
journalctl -u hangzhouwan-video.service --since "-30 min" --no-pager -o short-iso \
  > /tmp/video-$(date +%Y%m%d%H%M).log
# 仅回显统计与首/末样本
wc -l /tmp/video-*.log
head -5 /tmp/video-*.log; echo '...'; tail -5 /tmp/video-*.log
```

### 8.2 VPU Heap 查询与趋势记录

```bash
# VPU 设备内存（heap0=DDR、heap2=VPU 2048MB）。重连泄漏主要看 heap2 used 是否随轮次单调增长
bm-smi -noloop

# 趋势记录（重连前后各采样一次，对比 used 增量）
echo "before: $(bm-smi -noloop | grep -A2 'memory' | tail -1)"
# ... 触发重连 ...
echo "after:  $(bm-smi -noloop | grep -A2 'memory' | tail -1)"
```

判定：正常重连 heap2 used **持平或下降**；每轮 +39.5MB 单调增长即为 bm_image 池孤儿化泄漏。

### 8.3 常见错误与判定表

| 关键词 | 含义 | 判定 | 处置 |
|---|---|---|---|
| `bm_alloc_gmem failed` | VPU 设备内存分配失败 | heap 耗尽/碎片化 | 查 bm-smi heap2；若持续，Video-only 重启；复发则停止反复重启，申请受控整机重启 |
| `AllocateDecFrameBuffer fail` | 解码器帧缓冲分配失败 | heap 不足，新解码器无法开池 | 同上；确认旧解码器已释放 |
| `BMVidDecSeqInitW5 failed 0xffffffff` | 解码序列初始化失败 | heap 耗尽或残留解码器占用 | 重启进程回收；检查 inflight 是否归零 |
| `VPU_DecOpen failed 0x11` | 解码器打开失败(ENOMEM) | VPU 堆接近耗尽 | 进程退出由内核回收 heap；确认 StartLimit 未触发 |
| `free gmem addr 0xfffffffff is invalide` | 无效地址释放 | 释放了 Codec 内部地址（旧 bug） | 已由"仅 av_frame_unref 回收"修复；若再现需排查是否绕过 release() |
| `DEVICE_RESOURCE_FATAL` | 资源致命熔断 | ENOMEM 升级，退出码 70 | systemd 自动重启；确认 heap 恢复后再观察 |
| `RTSP_RECONNECT` | RTSP 断流交管线协调 | 正常重连流程 | 等待重连成功；若重连计数异常增长查网络 |
| `RTMP重连 ... 仅重建muxer` | RTMP 网络重连 | 编码器保留，无 VPU churn | 正常；若编码器重建则查是否 ENOMEM |
| `在途帧未归零` | 重连前 inflight 未排空 | 处理/编码线程持帧超时 | 检查 RTMP 是否阻塞 write；超时退出 70 由 systemd 恢复 |
| `降级状态: stream_down` / hzwctl `状态: DEGRADED` | 单路不可用 | B 路 RTSP 断开等 | 按单路降级策略处置，A 路不受影响 |

---

## 9. 查看进程和资源

```bash
# 按内存排序的进程
ps -eo pid,ppid,%cpu,%mem,rss,vsz,nlwp,etime,cmd --sort=-rss | head -30

# 内存
free -h

# 磁盘
df -h

# TPU
bm-smi -noloop

# 负载
cat /proc/loadavg

# 网络
ss -tpn
```

查看服务 PID 和重启次数：

```bash
systemctl show hangzhouwan-business.service \
  -p MainPID -p ActiveState -p SubState -p NRestarts

systemctl show hangzhouwan-video.service \
  -p MainPID -p ActiveState -p SubState -p NRestarts
```

查看 FD 和线程：

```bash
VIDEO_PID=$(systemctl show hangzhouwan-video.service -p MainPID --value)
ls "/proc/$VIDEO_PID/fd" | wc -l
grep -E 'Threads|VmRSS|VmSize' "/proc/$VIDEO_PID/status"
```

### 生产环境资源基线（2.6G 内存 / 无 Swap）

| 指标 | 正常范围 | 说明 |
|---|---|---|
| Video RSS | 30-40 MB | C++ 进程，稳定不增长 |
| Business RSS | 100-120 MB | Python 进程，含 AIS 缓存 |
| Video FD | <50 | 24 `/dev` + 9 socket + 少量文件 |
| Video 线程 | 17 | 固定 |
| Business 线程 | 14 | 固定 |
| 根分区使用 | <80% | journald 限制 50M，JSONL 轮转 50M×10 |
| /opt 使用 | <30% | Release 制品 |
| loadavg | <3.0 | 双路推理满载约 1 核 |

---

## 10. 查看 Socket

```bash
ls -l /run/hangzhouwan/

test -S /run/hangzhouwan/business.sock && echo "business socket OK"
test -S /run/hangzhouwan/video-health.sock && echo "video health socket OK"
```

---

## 11. 配置检查

```bash
sudo ls -l /etc/hangzhouwan/
sudo systemctl cat hangzhouwan-business.service
sudo systemctl cat hangzhouwan-video.service
```

修改配置后，先预检再重启：

```bash
/opt/hangzhouwan/current/bin/hzwctl preflight \
  --release /opt/hangzhouwan/current \
  --config /etc/hangzhouwan/application.yaml \
  --runtime

sudo systemctl restart hangzhouwan.target
```

> 配置文件不含明文密码。RTSP/RTMP/MQTT 凭据通过环境变量注入（`/etc/hangzhouwan/business.env` 和 `video.env`）。

### 当前 application.yaml 关键配置

```yaml
# 推流：正式地址（forbidden_formal_outputs 已清空）
deploy:
  forbidden_formal_outputs: []

# AIS 订阅：通配符（base_2250/2251 已离线）
mqtt:
  topics: ["upAIS/#"]

# 推理
inference:
  model_path: /opt/hangzhouwan/current/models/yolov7_ship_1684_f32.bmodel
  confidence_threshold: 0.1
  iou_threshold: 0.1

# 坐标预测
business:
  coordinate_mode: sklearn
  model_a_path: /opt/hangzhouwan/current/models/0121_random_forest_model.pkl
  model_b_path: /opt/hangzhouwan/current/models/beishang_x-l.pkl
  ais_max_distance_m:
    A: 500
    B: 500
```

---

## 12. Release 版本和链接

```bash
readlink -f /opt/hangzhouwan/current
readlink -f /opt/hangzhouwan/previous
cat /opt/hangzhouwan/current/VERSION
cat /opt/hangzhouwan/current/manifest.json
```

---

## 13. 升级 Release

```bash
# 1. 验证候选 Release
bash tools/release/verify_release.sh <candidate-release>

# 2. 安装并核对候选 systemd 单元（不自动启动）
sudo bash tools/release/install_release.sh <candidate-release>

# 3. 激活（含预检、原子切换、重启、readiness、60s smoke、自动回滚）
sudo bash tools/release/activate_release.sh <candidate-release>
```

激活流程：
1. **verify**：SHA256 + manifest 完整性校验
2. **unit drift**：确认候选 unit 已安装且与 `/etc/systemd/system` 完全一致
3. **preflight**：离线 + 激活条件预检（不读旧 current）
4. **离线 smoke**：VERSION 可读、dual_stream_app 可执行
5. **原子切换**：`mv -T` 原子替换 current（旧 current 保存为 previous）
6. **restart**：重启 `hangzhouwan.target`
7. **wait-business**：等待 Business readiness（30s 超时）
8. **wait-health**：严格等待 A/B 全链路健康
9. **60s smoke**：持续 60 秒冒烟检查
10. **失败自动回滚**：任一步骤失败，current 切回 previous，重启服务

确认升级成功：

```bash
readlink -f /opt/hangzhouwan/current   # 应指向新 Release
/opt/hangzhouwan/current/bin/hzwctl status
```

> ⚠️ cleanup-46a151c 已是纳入全部热修补丁的正式重建 Release（见第 18 节）；旧 current `202607221953-3dfcf4e` 为就地热修、manifest SHA256 失效，激活 cleanup 后即替换，不会丢失 decoder.py、sklearn_predictor.py 修复和 e2e_p95 健康接口修复。

### 受控激活与上线顺序（cleanup Release）

cleanup Release 构建依赖 Git 源码（`46a151c`）与固定 SHA256 的外部权重资产（2 个 PKL + bmodel，见 `assets/README.md`）。上线须按以下顺序，严禁跳步：

1. **完整性异常审计**：核实候选 Release 与现行生产二进制/热修一致性、外部权重可追溯。
2. **人工批准维护窗口**：确认回滚目标可靠、维护时段、通知相关方。
3. **受控激活 cleanup**：`sudo bash tools/release/activate_release.sh /opt/hangzhouwan/releases/cleanup-46a151c`（原子切换 + 自动回滚）。
4. **5–10 分钟快速健康验证**：`hzwctl status` / health / 双路推流 / MQTT=AIS / Socket。
5. **回滚能力验证**：确认 `previous` 指向激活前 current（热修版），可 `rollback_release.sh` 回退。
6. **必要时重新激活 cleanup**：回滚演练后再次激活 cleanup 作为正式目标。
7. **L4 24 小时长测**：按 `manual-long-run-guide.md`。
8. **Windows 回切演练**：确认停止 systemd 后 Windows 旧服务可接管。
9. **systemd enable**：`sudo systemctl enable hangzhouwan.target`（仅 L4 + 回切演练通过后）。
10. **至少 7 天稳定观察**：监控健康度、重连、资源。

---

## 14. 手动回滚

```bash
sudo bash /opt/hangzhouwan/current/tools/release/rollback_release.sh
```

回滚后验证：

```bash
/opt/hangzhouwan/current/bin/hzwctl wait-business --timeout 30
/opt/hangzhouwan/current/bin/hzwctl wait-video --timeout 60
/opt/hangzhouwan/current/bin/hzwctl status
```

---

## 15. 回切到 Windows 旧服务

如果 BM1684 系统出现不可恢复的故障，回切到 Windows 旧服务：

1. 在 Windows 机器上启动旧服务，推流到正式 RTMP 地址
2. 停止 BM1684：
   ```bash
   sudo systemctl stop hangzhouwan.target
   ```
3. 确认 Windows 推流正常，正式地址可拉流
4. BM1684 保留待修复，不影响 Windows 服务

---

## 16. 常见问题排查

### A 路无输出

- **现象**：`hzwctl health` 显示 A 路 `rtmp_connected: false` 或 `output_fps: 0`
- **检查**：`journalctl -u hangzhouwan-video.service | grep 'A.*RTSP'`
- **可能原因**：RTSP 连接失败、RTMP 服务器不可达
- **恢复**：检查 `video.env` 中的 `STREAM_A_INPUT_URL` 和 `STREAM_A_OUTPUT_URL`，验证 RTMP 服务器
- **影响**：该路无输出，另一路不受影响

### B 路 400 Bad Request

- **现象**：B 路 `rtsp_connected: false`，日志显示 `Server returned 400 Bad Request`
- **检查**：`journalctl -u hangzhouwan-video.service | grep '400'`
- **可能原因**：摄像头端 `Channels/301` 通道固件问题
- **恢复**：重启 Video 服务触发重连；若持续失败需现场检查摄像头配置
- **替代通道**：`101/201/401/501/701/801` 可用（需确认画面方向）
- **影响**：仅影响 B 路，A 路不受影响

### MQTT 未连接

- **现象**：`hzwctl health` 显示 `mqtt_connected: false`
- **检查**：`journalctl -u hangzhouwan-business.service | grep MQTT`
- **可能原因**：MQTT broker 不可达、凭据错误、网络问题
- **恢复**：检查 `business.env` 中的 `AIS_MQTT_*` 变量
- **影响 Video**：不影响推流，但 AIS 匹配不可用

### AIS 缓存为 0

- **现象**：`hzwctl health` 显示 `ais_cache_count: 0`
- **检查**：`journalctl -u hangzhouwan-business.service | grep AIS`
- **可能原因**：MQTT 未连接、通配符订阅未生效、AIS broker 无数据
- **恢复**：确认 `business.env` 中 `AIS_MQTT_TOPICS=upAIS/#`；确认 MQTT 连接正常
- **影响**：坐标预测正常（COORD_ONLY），但无 AIS 匹配

### AIS 数据有缓存但无匹配

- **现象**：AIS 缓存 >0 但 JSONL 中 `ais_matched: false`
- **检查**：确认 AIS 船舶是否在摄像头视野 500m 范围内
- **可能原因**：AIS 船舶距离过远（匹配半径 500m），或当前视野内无 AIS 船舶
- **恢复**：属正常行为，有 AIS 船舶经过视野时自动匹配
- **影响**：无

### 坐标模型加载失败

- **现象**：Business 启动日志显示模型加载错误
- **检查**：确认 `/opt/hangzhouwan/current/models/` 下模型文件存在
- **恢复**：重新构建 Release 或手动复制模型
- **影响 Video**：Video 降级为 DETECTION_ONLY

### bmodel 加载失败

- **现象**：Video 日志显示 `bmodel 加载失败` 或 `bmrt` 错误
- **检查**：确认 `/opt/hangzhouwan/current/models/yolov7_ship_1684_f32.bmodel` 存在且 SHA256 匹配
- **恢复**：重新构建 Release
- **影响**：Video 无法启动

### h264_bm 不存在

- **现象**：preflight 显示 `FFmpeg h264_bm 解码器 未找到`
- **检查**：`ffmpeg -decoders | grep h264_bm`
- **可能原因**：sophon-ffmpeg 未安装或 PATH 不含 sophon bin
- **恢复**：确认 `/opt/sophon/sophon-ffmpeg_0.8.0/` 存在，hzwctl 已自动添加 sophon bin 到 PATH
- **影响**：Video 无法使用硬件编解码

### 动态库 not found

- **现象**：`ldd dual_stream_app` 显示 `not found`
- **检查**：`ldd /opt/hangzhouwan/current/bin/dual_stream_app`
- **可能原因**：libsophon 或 sophon-ffmpeg 路径变更
- **恢复**：确认 `/opt/sophon/libsophon-0.4.9/lib` 和 `/opt/sophon/sophon-ffmpeg_0.8.0/lib` 存在；二进制已嵌入 RUNPATH
- **影响**：Video 无法启动

### TPU 设备不可用

- **现象**：`ls /dev/bm-tpu*` 无输出或 bmodel 加载失败
- **检查**：`ls /dev/bm-tpu* /dev/bm-sophon*`、`bm-smi -noloop`
- **可能原因**：驱动未加载、设备权限不足
- **恢复**：重启工控机或重新加载驱动
- **影响**：Video 无法推理

### Video 进入 DETECTION_ONLY

- **现象**：`hzwctl health` 显示 `business_state: DETECTION_ONLY`
- **检查**：确认 Business 服务 active 且 business.sock 存在；查看 JSONL 是否有坐标（`coordinate_valid: true`）
- **可能原因**：Business 未运行（真降级）或当前帧无检测目标（正常，坐标预测仅在检测到船时触发）
- **恢复**：若 Business 未运行则重启 Business；若 JSONL 有坐标则属正常
- **影响**：不影响另一条流

### 磁盘空间不足

- **现象**：日志显示磁盘告警或服务异常
- **检查**：`df -h`、`du -sh /var/log/ /var/lib/hangzhouwan/`
- **可能原因**：`kern.log` VPU clock 日志刷屏、journal 累积、JSONL 未轮转
- **恢复**：
  ```bash
  sudo truncate -s 0 /var/log/kern.log
  sudo journalctl --vacuum-size=50M
  ```
- **影响**：可能导致日志写入失败

### systemd 重启风暴

- **现象**：`NRestarts` 快速增长，服务反复失败重启
- **检查**：`systemctl show hangzhouwan-video.service -p NRestarts`
- **恢复**：`sudo systemctl reset-failed hangzhouwan-video.service` 后排查根因
- **影响**：服务不可用

### Release 激活失败

- **现象**：`activate_release.sh` 返回非零退出码
- **检查**：查看激活脚本输出（结果码：2=verify失败, 3=preflight失败, 4=smoke失败, 5=business未就绪, 6=video未就绪, 7=60s smoke失败）
- **恢复**：自动回滚已触发；确认 current 指向有效 Release 后 `sudo systemctl start hangzhouwan.target`
- **影响**：服务短暂中断后恢复

### 自动回滚失败

- **现象**：回滚后 Business/Video 仍未就绪
- **检查**：`readlink -f /opt/hangzhouwan/current`、`readlink -f /opt/hangzhouwan/previous`
- **恢复**：手动 `sudo ln -sfn <valid-release> /opt/hangzhouwan/current && sudo systemctl reset-failed hangzhouwan.target && sudo systemctl start hangzhouwan.target`
- **影响**：服务不可用，需人工干预

---

## 17. 开机启动说明

> ⚠️ **当前阶段不自动 enable 开机启动。**

只有在完成 **24 小时长测**和 **Windows 回切演练**后才允许执行：

```bash
sudo systemctl enable hangzhouwan.target
```

取消开机启动：

```bash
sudo systemctl disable hangzhouwan.target
```

> **禁止在完成 L4 24 小时长测和 Windows 回切演练前 enable。**

---

## 18. 热修补丁清单

> 以下补丁已就地应用到 Release `202607221953-3dfcf4e`，源码已提交到 Git 工作区。
> 下次正式构建 Release 时需确保这些补丁包含在内。

### 18.1 AIS 解码器增加 JSON 格式支持

- **文件**：`services/business_enrichment/ais/decoder.py`
- **问题**：`upAIS/dtu_shui_yu` 主题发送 JSON 格式 AIS 数据（`{"mmsi":..., "longitude":..., "latitude":...}`），原解码器仅支持 AIVDM NMEA 格式
- **修复**：`PyAisDecoder` 新增 `_decode_json()` 方法，自动检测 `{` 开头的 payload 并解析 JSON

### 18.2 AIS 订阅改用通配符

- **文件**：`/etc/hangzhouwan/business.env`、`/etc/hangzhouwan/application.yaml`
- **问题**：原订阅 `upAIS/base_2250`、`upAIS/base_2251` 基站已离线，60 秒内 0 条消息
- **修复**：改用 `upAIS/#` 通配符订阅所有 AIS 主题，缓存上限 500 + 30 秒超时自动清理

### 18.3 sklearn 特征名警告消除

- **文件**：`services/business_enrichment/coordinate/sklearn_predictor.py`
- **问题**：模型训练时用了 DataFrame 特征名，预测时传 numpy 数组导致每次推理刷 `UserWarning`
- **修复**：`load()` 后 `delattr(model, "feature_names_in_")` 消除警告

### 18.4 健康接口 e2e_p95_ms 修复

- **文件**：`include/monitoring/pipeline_metrics.h`、`src/application/dual_stream_application.cpp`
- **问题**：`PipelineMetrics::p95()` 是 private，健康状态 `e2e_p95_ms` 恒为 0
- **修复**：新增 public `e2e_p95()`/`infer_p95()`/`queue_length()` 访问器（带 mutex），在健康状态中填充实际值

### 18.5 推流地址切换为正式地址

- **文件**：`/etc/hangzhouwan/video.env`、`/etc/hangzhouwan/application.yaml`
- **变更**：`STREAM_*_OUTPUT_URL` 去掉 `_bm1684` 后缀，`forbidden_formal_outputs` 清空为 `[]`
- **说明**：BM1684 已替代 Windows 旧服务，直接推流到正式 RTMP 地址

### 18.6 系统级优化

- **journald 限制**：`/etc/systemd/journald.conf.d/size.conf` 配置 `SystemMaxUse=50M`
- **Docker 移除**：生产环境已卸载 Docker（无容器运行），释放磁盘 230MB + 内存 37MB
- **Cursor Server 移除**：生产环境已移除远程开发工具，释放内存 670MB + 磁盘 200MB
- **BSP 安装包清理**：`/home/linaro/bsp-debs` 已删除，释放 273MB

---

### 18.7 融合信息文字叠加 + 船名解码 + 健康状态稳定

- **问题**：BMCV 绘制路径仅画彩色矩形（`bmcv_image_draw_rectangle`），无融合文字，
  推流画面只有检测框、无 MMSI/速度等融合信息；健康状态在无检测帧时闪烁为
  `DETECTION_ONLY`；AIS type-5/24 静态报告（船名）被丢弃，多分片 AIVDM 无法解码。
- **修复**：
  - `src/image_io/jpeg_io.cpp`/`.h`：新增 `draw_label_nv12()`，在 NV12 Y 分量上直接
    绘制黑底白字标签（UV 置中性 128），并扩展内置 5x7 位图字体（A-Z、`:`、`-`，
    大小写不敏感）。
  - `src/pipeline/single_stream_pipeline.cpp`：BMCV 绘制分支在画矩形后对每个检测框
    调用 `draw_label_nv12`，标签为 `MMSI:<mmsi> S:<speed> [ship_name]`（仅 ASCII 船名）。
  - `src/business/business_enrichment_client.cpp`：`enrich()` 空结果（本帧无检测框）时
    保持当前状态，不再误判为 `DETECTION_ONLY`，消除健康状态闪烁。
  - `services/business_enrichment/ais/decoder.py`：解码 type-5/24 提取 `shipname`；
    `_decode_json()` 提取 `dtu_shui_yu` 的 `shipName`。
  - `services/business_enrichment/ais/subscriber.py`：多分片 AIVDM 重组（按 seq/channel
    缓冲，齐片后拼接 payload 解码），未齐分片不计入解析统计。
  - `services/business_enrichment/ais/store.py`：`AisRecord` 增加 `ship_name`，新增
    `_names` 缓存与 `update_ship_name()`，位置更新合并已知船名；type-5 计为解析成功
    （fail 率从 ~15% 降至 ~6%）。
- **部署**：就地热修补丁至 `/opt/hangzhouwan/current`（二进制 + Python 模块），
  备份在 `/var/log/hangzhouwan/hotfix-backup-20260723114513/`。
- **已知限制**：CJK 船名在叠加层以 `MMSI+速度` 显示（仅 ASCII 船名可渲染），
  完整中文叠加需 CJK 字体 + FreeType（后续）。船名需等 type-5 静态报告到达（约 6 分钟周期）。

## 19. 生产环境系统配置

### journald 日志限制

```bash
# /etc/systemd/journald.conf.d/size.conf
[Journal]
SystemMaxUse=50M
MaxFileSec=1week
```

### 磁盘空间维护

根分区 5.8G，已优化至 71% 使用率。定期检查：

```bash
df -h /
du -sh /var/log/ /var/lib/hangzhouwan/
```

如 `kern.log` 膨胀（VPU clock 日志刷屏）：

```bash
sudo truncate -s 0 /var/log/kern.log
sudo journalctl --vacuum-size=50M
```

### 已移除的非生产组件

| 组件 | 原因 | 释放 |
|---|---|---|
| Docker | 无容器运行 | 磁盘 230M + 内存 37M |
| Cursor Server | 远程开发工具，非生产 | 磁盘 200M + 内存 670M |
| BSP 安装包 | 安装后无用 | 磁盘 273M |
