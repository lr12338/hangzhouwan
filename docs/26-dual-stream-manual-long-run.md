# 阶段4.4 双路人工长时测试指南

## 前置条件

1. 已构建 `build/dual_stream_app`
2. 环境变量已设置：
   - `STREAM_A_INPUT_URL` / `STREAM_A_OUTPUT_URL`
   - `STREAM_B_INPUT_URL` / `STREAM_B_OUTPUT_URL`
3. TPU 和系统内存正常（`bm-smi -noloop`、`free -h`）
4. 无残留进程（`pgrep -af 'dual_stream|business_sidecar'`）

## 执行

```bash
# 30 分钟测试
./tools/dual_stream/dual_full_stack_stability.sh 30

# 2 小时测试
./tools/dual_stream/dual_full_stack_stability.sh 120
```

## 监控命令

在另一个终端执行：

```bash
# 实时日志
tail -f artifacts/internal-development/dual_full_stack.log

# TPU 状态
watch -n 5 bm-smi -noloop

# 内存状态
watch -n 5 free -h

# 进程资源
watch -n 5 "ps -eo pid,ppid,%cpu,%mem,rss,vsz,nlwp,cmd --sort=-rss | head -20"

# 进程存活
watch -n 5 "pgrep -af 'dual_stream_app|business_sidecar'"

# 指标 CSV
tail -f artifacts/internal-development/dual_metrics.csv
```

## 验收标准

| 指标 | 30分钟 | 2小时 |
|------|--------|-------|
| RSS 增长 | <100MB | <200MB |
| TPU 内存 | 无持续增长 | 无持续增长 |
| MQTT 缓存 | 受控 | 受控 |
| 文件描述符 | 无持续增长 | 无持续增长 |
| A/B 输出 fps | 无长期下降 | 无长期下降 |
| A/B RTMP | 持续可见 | 持续可见 |
| 重连后恢复 | 自动恢复 | 自动恢复 |
| 残留进程 | 无 | 无 |

## 测试结束检查

```bash
# 残留进程
pgrep -af 'dual_stream_app|business_sidecar' || echo "无残留"

# 内核日志
dmesg | grep -Ei 'oom|killed process|bm|vpu|error' | tail -100

# JSONL 事件计数
wc -l artifacts/internal-development/stream_*_events.jsonl

# 指标 CSV 最后几行
tail -5 artifacts/internal-development/dual_metrics.csv
```

## 注意事项

- Agent 不得自动执行超过 300 秒的测试
- 30分钟/2小时测试必须人工监控
- 如果 RSS 或 TPU 内存持续单调增长，应停止测试并报告
- 如果任一路 RTMP 持续无法恢复，应停止测试
- 阈值应结合 300 秒真实基线进一步校准
