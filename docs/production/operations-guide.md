# 杭州湾双路检测系统 · 生产运维手册

> 本文档为 BM1684 板端生产部署的主要操作参考，可直接复制执行。
> 适用版本：阶段7 Release（`202607221953-3dfcf4e` 及之后）。
> **红线：不自动 enable 开机启动；不关闭 Windows 旧服务；不覆盖 Windows 正式 RTMP 地址。**

---

## 1. 系统组成

```
hangzhouwan.target
├── hangzhouwan-business.service   (业务增强 Sidecar)
└── hangzhouwan-video.service      (双路视频管线)
```

- **Business**：Python sidecar，负责坐标预测（sklearn）、MQTT AIS 订阅/缓存、视觉-AIS 匹配、JSONL 事件输出。
- **Video**：C++ `dual_stream_app`，负责双路 RTSP 接入、h264_bm 硬解、BMCV 预处理、BMRuntime 推理、禁区过滤、融合快照绘制、h264_bm 硬编、灰度 RTMP 推流。
- 两者通过 Unix Domain Socket 通信。
- Business 异常时 Video 自动降级为 `DETECTION_ONLY`（仅检测，无坐标/AIS），Business 恢复后自动恢复业务融合。

### 依赖关系

- Video `Wants=hangzhouwan-business.service`（启动 Video 时一并拉起 Business，但 Business 故障不连带停止 Video）。
- Video `After=hangzhouwan-business.service`（保证 Business 先启动）。
- Video `ExecStartPre=hzwctl wait-business --timeout 30`（等 Business readiness 通过后才启动 Video 主程序）。
- 共享运行目录 `/run/hangzhouwan` 由 `tmpfiles.d` 统一管理，不随任一服务停止而删除。

---

## 2. 目录结构

| 路径 | 说明 |
|---|---|
| `/opt/hangzhouwan/current` | 当前激活 Release 软链接 |
| `/opt/hangzhouwan/previous` | 上一版本 Release 软链接（回滚目标） |
| `/opt/hangzhouwan/releases/<version>-<commit>/` | 不可变 Release 制品目录 |
| `/etc/hangzhouwan/application.yaml` | 唯一权威配置（权限 640 root:linaro） |
| `/etc/hangzhouwan/business.env` | Business 环境变量（MQTT 凭据等，权限 640） |
| `/etc/hangzhouwan/video.env` | Video 环境变量（RTSP/RTMP URL 等，权限 640） |
| `/etc/tmpfiles.d/hangzhouwan.conf` | 共享运行目录 tmpfiles.d 配置 |
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

---

## 3. 启动服务

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

## 4. 停止服务

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

## 5. 重启服务

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

## 6. 查看 systemd 状态

```bash
systemctl status hangzhouwan.target --no-pager -l
systemctl status hangzhouwan-business.service --no-pager -l
systemctl status hangzhouwan-video.service --no-pager -l
```

精简状态：

```bash
systemctl is-active hangzhouwan-business.service
systemctl is-active hangzhouwan-video.service
systemctl is-failed hangzhouwan-business.service
systemctl is-failed hangzhouwan-video.service
```

---

## 7. 使用 hzwctl 检查状态

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

## 15. 收集诊断包

```bash
/opt/hangzhouwan/current/bin/hzwctl collect-diagnostics
```

诊断包保存在 `/var/log/hangzhouwan/diagnostics/diag_<timestamp>.txt`，包含：
- systemctl status（Business/Video/target）
- journal 最近 100 行日志
- Sidecar 健康状态
- Video 健康 Socket 状态
- 进程列表
- 磁盘空间
- TPU 状态

> **诊断包不得提交 Git**。`/var/log/hangzhouwan/` 已在 `.gitignore` 中排除。

---

## 16. 常见故障处理

### Business 启动失败

- **现象**：`systemctl is-active hangzhouwan-business.service` 返回 `failed`
- **检查**：`journalctl -u hangzhouwan-business.service -n 50 --no-pager`
- **可能原因**：MQTT 不可达、坐标模型加载失败、Python 依赖缺失、配置文件错误
- **恢复**：修复后 `sudo systemctl restart hangzhouwan-business.service`
- **影响 Video**：Video 降级为 DETECTION_ONLY，推流不中断

### Video readiness 失败

- **现象**：`hzwctl wait-video` 超时
- **检查**：`journalctl -u hangzhouwan-video.service -n 50 --no-pager`
- **可能原因**：Business 未就绪（ExecStartPre 失败）、bmodel 加载失败、TPU 不可用
- **恢复**：先确保 Business 就绪，再 `sudo systemctl restart hangzhouwan-video.service`
- **影响 Business**：不影响

### RTSP 连接失败

- **现象**：日志显示 `RTSP 连接失败` 或 `stimeout`
- **检查**：`journalctl -u hangzhouwan-video.service | grep RTSP`
- **可能原因**：网络不通、摄像头离线、凭据错误、RTSP URL 变更
- **恢复**：检查 `/etc/hangzhouwan/video.env` 中的 `STREAM_*_INPUT_URL`，验证网络连通性
- **影响**：该路无输出，另一路不受影响

### RTMP 推流失败

- **现象**：日志显示 `RTMP 失败` 或重连
- **检查**：`journalctl -u hangzhouwan-video.service | grep RTMP`
- **可能原因**：RTMP 服务器不可达、推流 Key 错误、端口被占用
- **恢复**：检查 `STREAM_*_OUTPUT_URL`，验证 RTMP 服务器
- **影响**：该路无输出，另一路不受影响

### MQTT 未连接

- **现象**：`hzwctl health` 显示 `mqtt_connected: false`
- **检查**：`journalctl -u hangzhouwan-business.service | grep MQTT`
- **可能原因**：MQTT broker 不可达、凭据错误、网络问题
- **恢复**：检查 `business.env` 中的 `AIS_MQTT_*` 变量
- **影响 Video**：不影响推流，但 AIS 匹配不可用

### AIS 缓存为 0

- **现象**：`hzwctl health` 显示 `ais_cache_count: 0`
- **检查**：`journalctl -u hangzhouwan-business.service | grep AIS`
- **可能原因**：MQTT 未连接、当前无船经过、AIS 主题错误
- **恢复**：确认 MQTT 连接正常，等待有船经过
- **影响**：坐标预测正常（COORD_ONLY），但无 AIS 匹配

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

### B 路频繁重连

- **现象**：B 路 `rtsp_reconnects` 持续增加
- **检查**：`journalctl -u hangzhouwan-video.service | grep 'B.*RTSP'`
- **可能原因**：B 路摄像头返回 400 Bad Request（已知问题）或网络不稳定
- **恢复**：检查摄像头状态和网络
- **影响**：仅影响 B 路，A 路不受影响

### Video 进入 DETECTION_ONLY

- **现象**：`hzwctl health` 显示 `business_state: DETECTION_ONLY`
- **检查**：确认 Business 服务 active 且 business.sock 存在；查看 JSONL 是否有坐标（`coordinate_valid: true`）
- **可能原因**：Business 未运行（真降级）或当前帧无检测目标（正常，坐标预测仅在检测到船时触发）
- **恢复**：若 Business 未运行则重启 Business；若 JSONL 有坐标则属正常
- **影响**：不影响另一条流

### 磁盘空间不足

- **现象**：日志显示磁盘告警或服务异常
- **检查**：`df -h`、`du -sh /var/log/hangzhouwan/ /var/lib/hangzhouwan/`
- **恢复**：清理旧诊断包和 JSONL 文件
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
