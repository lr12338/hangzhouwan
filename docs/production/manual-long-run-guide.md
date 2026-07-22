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
| 磁盘 | `df -h /opt` | >500MB |
| 日志轮转 | `ls -la /var/log/hangzhouwan/` | 轮转生效 |
| JSONL | `python3 -c "import json; [json.loads(l) for l in open('/var/lib/hangzhouwan/stream_A_events.jsonl')]"` | 全部合法 |
| AIS 缓存 | `hzwctl health` | 有船时 >0 |
| A/B fps | `hzwctl status` | A≥9fps, B≥5fps |
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
