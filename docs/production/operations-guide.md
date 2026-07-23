# 杭州湾双路检测系统 · 生产运维手册

> 本文档为 BM1684 板端生产部署的主要操作参考，可直接复制执行。
> 适用版本：阶段7 Release（`202607221953-3dfcf4e`，含热修补丁）。
> **当前状态：已替代 Windows 旧服务，双路推流正式地址，生产运行中。**

---

## 1. 系统组成

```
hangzhouwan.target
├── hangzhouwan-business.service   (业务增强 Sidecar)
└── hangzhouwan-video.service      (双路视频管线)
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
| `/opt/hangzhouwan/releases/<version>-<commit>/` | 不可变 Release 制品目录 |
| `/etc/hangzhouwan/application.yaml` | 唯一权威配置（权限 640 root:linaro） |
| `/etc/hangzhouwan/business.env` | Business 环境变量（MQTT 凭据等，权限 640） |
| `/etc/hangzhouwan/video.env` | Video 环境变量（RTSP/RTMP URL 等，权限 640） |
| `/etc/tmpfiles.d/hangzhouwan.conf` | 共享运行目录 tmpfiles.d 配置 |
| `/etc/systemd/journald.conf.d/size.conf` | journald 日志大小限制（50M） |
| `/run/hangzhouwan/business.sock` | Business Sidecar Unix Socket |
| `/run/hangzhouwan/video-health.sock` | Video 结构化健康接口 Socket |
| `/var/log/hangzhouwan/` | 日志和诊断包目录 |
| `/var/lib/hangzhouwan/` | JSONL 事件输出和状态数据目录 |

Release 目录结构：
```
/opt/hangzhouwan/releases/<version>-<commit>/
├── bin/dual_stream_app       # C++ 双路推理主程序
├── bin/hzwctl                # 运维工具
├── venv/                     # Python 虚拟环境（继承系统 site-packages）
├── models/                   # bmodel + 坐标模型
├── services/                 # Business sidecar Python 代码
├── systemd/                  # systemd 单元文件
├── tmpfiles.d/               # tmpfiles.d 配置
├── config/                   # 示例配置
├── VERSION                   # 版本信息
├── manifest.json             # 文件清单 + SHA256
└── sha256sum.txt             # 完整性校验
```

> ⚠️ 当前 Release 含热修补丁（见第 18 节），manifest.json SHA256 已失效。下次正式构建 Release 时需重新生成。

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
/opt/hangzhouwan/current/bin/hzwctl wait-business --timeout 30

# 2. 再启动 Video
sudo systemctl start hangzhouwan-video.service
/opt/hangzhouwan/current/bin/hzwctl wait-video --timeout 60
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

---

## 7. 查看系统状态

```bash
# 系统状态汇总（优先读 Video 健康接口）
/opt/hangzhouwan/current/bin/hzwctl status

# 健康状态
/opt/hangzhouwan/current/bin/hzwctl health

# 版本信息
/opt/hangzhouwan/current/bin/hzwctl version

# 运行时预检
/opt/hangzhouwan/current/bin/hzwctl preflight \
  --release /opt/hangzhouwan/current \
  --config /etc/hangzhouwan/application.yaml \
  --runtime
```

`status` 输出字段说明：
- **当前 Release**：版本号和路径
- **Business 服务**：active/inactive/failed + coordinate_mode + MQTT 状态 + AIS 缓存
- **Video 服务**：active/inactive/failed + A/B RTSP/RTMP 状态 + output/inference fps + 重连次数 + Business 降级状态 + RSS

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

# 2. 激活（含预检、原子切换、重启、readiness、60s smoke、自动回滚）
sudo bash tools/release/activate_release.sh <candidate-release>
```

激活流程：
1. **verify**：SHA256 + manifest 完整性校验
2. **preflight**：离线 + 激活条件预检（不读旧 current）
3. **离线 smoke**：VERSION 可读、dual_stream_app 可执行
4. **原子切换**：`mv -T` 原子替换 current（旧 current 保存为 previous）
5. **restart**：重启 `hangzhouwan.target`
6. **wait-business**：等待 Business readiness（30s 超时）
7. **wait-video**：等待 Video readiness（60s 超时）
8. **60s smoke**：持续 60 秒冒烟检查
9. **失败自动回滚**：任一步骤失败，current 切回 previous，重启服务

确认升级成功：

```bash
readlink -f /opt/hangzhouwan/current   # 应指向新 Release
/opt/hangzhouwan/current/bin/hzwctl status
```

> ⚠️ 升级时需重新构建含热修补丁的 Release（见第 18 节），否则会丢失 decoder.py、sklearn_predictor.py 修复和 e2e_p95 健康接口修复。

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
