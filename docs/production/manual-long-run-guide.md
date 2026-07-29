# 人工长时稳定性测试指南

> Agent 不执行超过 300 秒的自动测试。以下长时测试由人工执行，使用 release 目录和 systemd 服务（而非开发命令行）。

## 前置条件

1. Release 已构建并激活：
   ```bash
   bash tools/release/build_release.sh
   bash tools/release/activate_release.sh /opt/hangzhouwan/releases/<version>-<commit>
   ```
2. 预检通过：`hzwctl preflight`
3. 配置 `/etc/hangzhouwan/application.yaml` 已填入真实 RTSP/RTMP/MQTT
4. 环境变量文件 `/etc/hangzhouwan/business.env` 和 `video.env` 已填入凭据
5. systemd 单元已安装（未 enable 生产自动启动）

## 测试级别

| 级别 | 时长 | 门禁 | 观察重点 |
|------|------|------|----------|
| L1 | 30 分钟 | 无崩溃、fps 稳定 | 基本稳定性 |
| L2 | 2 小时 | 无内存泄漏、无积压 | 短时运行可靠性 |
| L3 | 8 小时 | 无累积退化 | 班次级稳定性 |
| L4 | 24 小时 | 全天候稳定 | 生产候选验收 |

## 执行步骤（每级别通用）

### 1. 启动服务

```bash
# 启动（不 enable 开机自启）
sudo systemctl start hangzhouwan.target

# 等待 business 就绪
hzwctl wait-business --timeout 30

# 等待 video 就绪
hzwctl wait-video --timeout 60
```

### 2. 定期检查（每 10 分钟）

```bash
# 系统状态
hzwctl status

# Sidecar 健康
hzwctl health
```

### 3. 检查项

| 检查项 | 命令 | 预期 |
|--------|------|------|
| service restart 次数 | `systemctl show hangzhouwan-video -p NRestarts` | 0 或极少 |
| RSS | `ps aux \| grep dual_stream_app \| awk '{print $6/1024"MB"}'` | 无持续增长 |
| TPU 内存 | `bm-smi` 或 `cat /sys/kernel/debug/bm1684/memory_usage` | 无持续增长 |
| FD | `ls /proc/$(pgrep dual_stream_app)/fd \| wc -l` | <100，无增长 |
| 线程 | `ps -o nlwp $(pgrep dual_stream_app)` | 稳定 |
| 磁盘 | `df -h / /data` | 根分区目标≥2GB，且不得低于1.5GB启动门禁 |
| 日志轮转 | `ls -la /var/log/hangzhouwan/` | 轮转生效 |
| JSONL | 对 `/data/hangzhouwan/events` 中当前 A/B JSONL 逐行执行 `json.loads` | 全部合法 |
| AIS 缓存 | `hzwctl health` | >0（upAIS/# 通配符订阅） |
| A/B fps | `hzwctl status` | A≥9fps, B≥9fps（双路均 10fps 目标） |
| RTSP 重连 | journalctl 日志 | 无频繁重连 |
| RTMP 重连 | journalctl 日志 | 无频繁重连 |
| business 降级 | JSONL enrichment_status | 偶发 DETECTION_ONLY 后恢复 |

### 4. 降级恢复验证

在测试中途停止 sidecar，验证：
```bash
sudo systemctl stop hangzhouwan-business.service
# 观察 1-2 分钟：video 推流不中断，JSONL 出现 DETECTION_ONLY
sudo systemctl start hangzhouwan-business.service
# 观察 1-2 分钟：JSONL enrichment_status 恢复 FULL/COORD_ONLY
```

### 5. 结束与诊断

```bash
# 收集诊断
hzwctl collect-diagnostics

# 停止服务
sudo systemctl stop hangzhouwan.target
```

## 各级别附加要求

### L1（30 分钟）
- 验证 A/B 双路均输出帧
- 验证 JSONL 格式合法
- 验证融合画面有彩色框（绿/黄/红）

### L2（2 小时）
- RSS 增长 <10%
- FD 不增长
- 无 service restart

### L3（8 小时）
- 累积 RTSP 重连 <10 次
- 累积 RTMP 重连 <5 次
- TPU 内存无泄漏
- 日志轮转生效

### L4（24 小时）
- 全部 L3 指标满足
- 降级恢复至少验证 3 次
- 诊断报告无异常
- **此级别通过后才可进入 Windows 灰度替换**

### 生产签收（72 小时）
- 仅在候选版本已经通过 L4 24 小时观察后开始
- A/B 平均输出均 ≥9fps，推理均 ≥4fps
- A/B 端到端 P95 均 <500ms
- 每路 RTSP+RTMP 重连尝试均 ≤2 次/小时
- RSS、TPU 内存、FD、线程及根盘余量无持续增长
- 任一必需流出现持续 FAILED、维护恢复未清除或根盘跌破 1.5GB，签收失败

---

## 指标采样模板

每 10 分钟执行以下采样并记录：

```bash
#!/bin/bash
# 长测指标采样脚本
TS=$(date '+%Y-%m-%d %H:%M:%S')
VPID=$(systemctl show hangzhouwan-video.service -p MainPID --value)
echo "=== $TS ==="
echo "--- hzwctl health ---"
/opt/hangzhouwan/current/bin/hzwctl health 2>&1 | grep -E 'status|business_state|rtsp|rtmp|output_fps|inference_fps|rss|reconnect'
echo "--- process ---"
echo "video FDs=$(ls /proc/$VPID/fd 2>/dev/null | wc -l) threads=$(grep Threads /proc/$VPID/status 2>/dev/null | awk '{print $2}')"
echo "video RSS=$(grep VmRSS /proc/$VPID/status 2>/dev/null | awk '{print $2/1024"MB"}')"
echo "--- systemd ---"
echo "business NRestarts=$(systemctl show hangzhouwan-business.service -p NRestarts --value)"
echo "video NRestarts=$(systemctl show hangzhouwan-video.service -p NRestarts --value)"
echo "--- journal e2e ---"
journalctl -u hangzhouwan-video.service --no-pager -n 5 2>&1 | grep 'P95' | tail -1
echo "--- disk ---"
df -h / /opt | tail -2
echo "--- JSONL count ---"
wc -l /var/lib/hangzhouwan/stream_A_events.jsonl 2>/dev/null
```

## 失败停止条件

出现以下任一情况立即停止测试并收集诊断：

1. `dual_stream_app` 进程消失或 core dump
2. `systemctl is-active` 返回 `failed` 且无法自动恢复
3. NRestarts 在 10 分钟内增长 >5（重启风暴）
4. RSS 持续增长且不回落（内存泄漏）
5. FD 数 >500 或持续增长
6. 根分区磁盘空间 <500MB
7. A 路 output_fps 持续 <5fps 超过 5 分钟
8. RTSP/RTMP 重连频率 >2 次/小时
9. TPU 内存持续增长

## 测试报告模板

```
## L<n> 长测报告

- 测试级别：L<n>（<时长>）
- 开始时间：
- 结束时间：
- Release 版本：
- 配置文件：/etc/hangzhouwan/application.yaml

### 结果

| 指标 | 开始值 | 结束值 | 变化 | 判定 |
|------|--------|--------|------|------|
| A output_fps | | | | |
| A inference_fps | | | | |
| B output_fps | | | | |
| RSS (MB) | | | | |
| FD 数 | | | | |
| 线程数 | | | | |
| business NRestarts | | | | |
| video NRestarts | | | | |
| RTSP 重连 (A) | | | | |
| RTMP 重连 (A) | | | | |
| 磁盘 /opt | | | | |

### 降级恢复验证

- 停止 Business 时间：
- 恢复 Business 时间：
- JSONL 恢复 COORD_ONLY 时间：
- Video 是否中断：

### 异常记录

（无异常 / 描述异常及处理）

### 诊断包

路径：/var/log/hangzhouwan/diagnostics/diag_<timestamp>.txt

### 结论

通过 / 不通过（原因）
```
