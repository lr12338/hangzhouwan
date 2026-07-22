# 阶段7 实装验证 + 人工长测执行方案

> **红线**：不自动 enable 正式服务；不停止 Windows 旧服务；不使用正式 RTMP 地址覆盖旧服务；每项测试不超过 300 秒。

## 前置条件

1. BM1684 目标板已编译 C++（含新增 `video_health_server.cpp`）
2. `/etc/hangzhouwan/application.yaml` 已配置真实流地址（环境变量注入，不入文件）
3. `/etc/hangzhouwan/business.env` 和 `video.env` 已配置
4. Windows 旧服务正常运行
5. 灰度 RTMP 输出地址**不同于** Windows 正式地址

## 一、T1-T10 短测（每项最长 300 秒）

### T1：候选 Release 静态预检

```bash
# 构建候选 Release
bash tools/release/build_release.sh candidate-v1

# 静态预检（不读 current，不检查运行时）
/opt/hangzhouwan/current/bin/hzwctl preflight \
  --release /opt/hangzhouwan/releases/candidate-v1-<commit> \
  --config /etc/hangzhouwan/application.yaml \
  --offline --activation
```

**通过条件**：全部 ✅，无 ❌

### T2：systemd-analyze verify

```bash
systemd-analyze verify \
  /etc/systemd/system/hangzhouwan-business.service \
  /etc/systemd/system/hangzhouwan-video.service \
  /etc/systemd/system/hangzhouwan.target
```

**通过条件**：无错误输出

### T3：Business 从 YAML 启动并 readiness 通过

```bash
sudo systemctl start hangzhouwan-business.service
/opt/hangzhouwan/current/bin/hzwctl wait-business --timeout 30
```

**通过条件**：`✅ business 就绪`

### T4：Video 等待 Business 后启动

```bash
sudo systemctl start hangzhouwan-video.service
/opt/hangzhouwan/current/bin/hzwctl wait-video --timeout 60
```

**通过条件**：`✅ video 就绪`

### T5：Video 健康 Socket

```bash
# 查询健康
python3 -c "
import socket, json
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/run/hangzhouwan/video-health.sock')
s.sendall((json.dumps({'action':'health'})+'\n').encode())
print(json.dumps(json.loads(s.recv(65536).split(b'\n')[0]), indent=2, ensure_ascii=False))
"
```

**通过条件**：返回 JSON 含 release/version/commit、A/B 流状态、fps、Business 状态

### T6：restart video 不影响 Business Socket

```bash
# 确认 socket 存在
ls -la /run/hangzhouwan/business.sock

# 重启 video
sudo systemctl restart hangzhouwan-video.service

# 确认 socket 仍在（RuntimeDirectory 仅 Business 拥有）
ls -la /run/hangzhouwan/business.sock
```

**通过条件**：business.sock 在 restart video 后仍存在

### T7：restart business 时 Video 降级并恢复

```bash
# 重启 business
sudo systemctl restart hangzhouwan-business.service

# 检查 video 降级（DETECTION_ONLY）
/opt/hangzhouwan/current/bin/hzwctl health | grep degradation

# 等待 business 恢复
sleep 10

# 检查 video 恢复融合（FULL）
/opt/hangzhouwan/current/bin/hzwctl health | grep business_state
```

**通过条件**：降级为 detection_only，恢复后 business_state=FULL

### T8：候选 Release 激活成功

```bash
bash tools/release/activate_release.sh \
  /opt/hangzhouwan/releases/candidate-v1-<commit> \
  --config /etc/hangzhouwan/application.yaml
```

**通过条件**：`结果码: 0 (成功)`，current 指向新 Release

### T9：错误候选 Release 自动回滚

```bash
# 创建一个损坏的候选（删除 bmodel）
cp -r /opt/hangzhouwan/releases/candidate-v1-<commit> /opt/hangzhouwan/releases/bad-release
rm /opt/hangzhouwan/releases/bad-release/models/yolov7_ship_1684_f32.bmodel

# 尝试激活（应失败并自动回滚）
bash tools/release/activate_release.sh /opt/hangzhouwan/releases/bad-release
echo "结果码: $?"

# 确认 current 仍指向之前的 Release
ls -la /opt/hangzhouwan/current
```

**通过条件**：结果码非 0，current 切回 previous

### T10：灰度双路 300 秒

```bash
# 启动双路（灰度 RTMP 地址，不覆盖 Windows）
sudo systemctl start hangzhouwan.target

# 运行 300 秒
timeout 300 /opt/hangzhouwan/current/bin/hzwctl smoke-test

# 确认灰度输出可见（灰度地址，非 Windows 正式地址）
/opt/hangzhouwan/current/bin/hzwctl status
```

**通过条件**：smoke-test 通过，灰度推流可见，不覆盖 Windows 正式输出

---

## 二、测试结束条件检查

```bash
# 1. 无残留测试进程
pgrep -f 'dual_stream_app' && echo "❌ 残留进程" || echo "✅ 无残留"

# 2. systemd 没有重启风暴
systemctl show hangzhouwan-business.service -p NRestarts --value
systemctl show hangzhouwan-video.service -p NRestarts --value
# NRestarts 应 < 10

# 3. current/previous 指向正确
ls -la /opt/hangzhouwan/current /opt/hangzhouwan/previous

# 4. 配置读取一致
/opt/hangzhouwan/current/bin/hzwctl preflight --offline

# 5. 业务融合实际启用
/opt/hangzhouwan/current/bin/hzwctl health | grep business_state
# 应为 FULL 或 COORD_ONLY

# 6. 健康接口返回真实指标
/opt/hangzhouwan/current/bin/hzwctl health

# 7. 灰度推流可见
/opt/hangzhouwan/current/bin/hzwctl status

# 8. 不 enable 开机启动
systemctl is-enabled hangzhouwan.target 2>/dev/null
# 应为 disabled 或 not-found

# 9. 不关闭 Windows 服务（人工确认）
```

---

## 三、人工长测方案

### L1：30 分钟

**目标**：验证基本稳定性

| 步骤 | 操作 | 检查 |
|------|------|------|
| 1 | `sudo systemctl start hangzhouwan.target` | 两服务启动 |
| 2 | 等待 30 分钟 | 定期 `hzwctl status` |
| 3 | 每 5 分钟记录 | A/B output_fps, inference_fps, RTSP/RTMP 重连次数 |
| 4 | 30 分钟后 `hzwctl health` | status=HEALTHY, business_state=FULL |
| 5 | `hzwctl collect-diagnostics` | 保存诊断 |

**通过条件**：
- 无崩溃（两服务 active 全程）
- A/B output_fps ≥ 7（HEALTHY）
- RTSP/RTMP 重连 ≤ 2 次
- RSS 无持续增长（内存泄漏）
- business_state=FULL（融合启用）

**失败处理**：停止服务，检查 journal，不继续 L2

### L2：2 小时

**目标**：验证中期稳定性 + 融合持续

| 步骤 | 操作 | 检查 |
|------|------|------|
| 1 | 启动服务 | 同 L1 |
| 2 | 等待 2 小时 | 每 15 分钟 `hzwctl status` |
| 3 | 1 小时时重启 business | Video 降级后恢复 |
| 4 | 1.5 小时时检查 MQTT | `hzwctl health` mqtt_connected=true |
| 5 | 2 小时后全量检查 | status, health, diagnostics |

**通过条件**：
- 无崩溃
- business 重启后 Video 正确降级并恢复
- MQTT 持续连接
- 磁盘未满（JSONL 日志轮转正常）
- e2e P95 < 500ms

**失败处理**：停止服务，保存诊断，不继续 L3

### L3：8 小时

**目标**：验证长时间稳定性 + 资源管理

| 步骤 | 操作 | 检查 |
|------|------|------|
| 1 | 启动服务 | 同 L1 |
| 2 | 等待 8 小时 | 每 30 分钟 `hzwctl status` |
| 3 | 每 2 小时检查磁盘 | `df -h /opt`、`du -sh /var/lib/hangzhouwan` |
| 4 | 每 2 小时检查 TPU | `bm-smi` |
| 5 | 4 小时时模拟 RTSP 断线 | 观察重连 |
| 6 | 8 小时后全量检查 | status, health, diagnostics |

**通过条件**：
- 无崩溃
- 无重启风暴（NRestarts < 10）
- 磁盘稳定（日志轮转生效）
- TPU 内存无泄漏
- RTSP 断线后自动重连
- business_state=FULL 全程

**失败处理**：停止服务，保存诊断，不继续 L4

### L4：24 小时

**目标**：最终门禁，可批准替换 Windows

| 步骤 | 操作 | 检查 |
|------|------|------|
| 1 | 启动服务 | 同 L1 |
| 2 | 等待 24 小时 | 每小时 `hzwctl status` |
| 3 | 12 小时时重启 video | business.sock 不被删除 |
| 4 | 18 小时时重启 business | Video 降级并恢复 |
| 5 | 24 小时后全量检查 | status, health, diagnostics, 磁盘, TPU |

**通过条件**：
- 无崩溃
- 所有 restart 测试通过
- A/B output_fps ≥ 7 全程
- business_state=FULL 全程
- 磁盘稳定
- TPU 内存稳定
- 真实 MQTT AIS 消息收到
- 真实 AIS 样本 A/B 各 ≥ 20

**通过后**：
- 可批准灰度替换 Windows
- 人工执行 `sudo systemctl enable hangzhouwan.target`（仅批准后）
- 停止 Windows 服务（仅批准后）

---

## 四、真实数据门禁

### MQTT AIS 验证

```bash
# 运行 MQTT 探测（300秒）
/opt/hangzhouwan/current/venv/bin/python3 -m services.business_enrichment.app \
  --config /etc/hangzhouwan/application.yaml \
  --mqtt-probe --seconds 300
```

**通过条件**：mqtt_connected=true, ais_decode_ok > 0

### AIS 人工样本验证

- A/B 各采集 ≥ 20 个检测帧
- 每个 JSONL 事件检查：longitude/latitude 非 0，ais_matched 有值
- 匹配率 ≥ 80%（有 AIS 数据时）

### Windows 回切演练

```bash
# 停止 systemd 服务
sudo systemctl stop hangzhouwan.target

# 确认 Windows 服务正常接管
# （人工检查 Windows 推流）

# 确认无残留进程
pgrep -f 'dual_stream_app' && echo "❌" || echo "✅"
```

---

## 五、最终结论判断

| 结论 | 条件 |
|------|------|
| 1. Release/systemd实装短测通过，可以进入人工长测 | T1-T10 全部通过 |
| 2. 配置或readiness存在问题，禁止长测 | T1-T4 任一失败 |
| 3. 激活/回滚存在问题，禁止替换Windows | T8 或 T9 失败 |
| 4. 灰度双路通过，30min/2h/8h/24h待人工 | T10 通过，L1-L4 待执行 |
| 5. 发现关键生产风险，禁止部署 | L1-L4 任一发现关键问题 |
