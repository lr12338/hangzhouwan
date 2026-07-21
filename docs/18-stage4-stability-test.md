# 阶段4：稳定性测试报告

> 状态：阶段4 本地单路视频功能通过；短时 300 秒验证通过；完整 30 分钟和 2 小时门禁待人工执行。

## 测试脚本

`tools/video_inference/stability_test.sh`

### 用法

```bash
./tools/video_inference/stability_test.sh [10|60|300|30|120] [--preprocess cpu|bmcv] [--draw-mode cpu|bmcv|none]
```

| 参数 | 时长 | 用途 | Agent 可执行 |
|------|------|------|-------------|
| 10 | 10 秒 | 功能验证 | ✅ |
| 60 | 60 秒 | 性能测试 | ✅ |
| 300 | 300 秒 | 短时稳定性 | ✅（最长） |
| 30 | 1800 秒 | 30 分钟中等 | ❌ 人工 |
| 120 | 7200 秒 | 2 小时门禁 | ❌ 人工 |

默认配置：`--preprocess cpu --draw-mode bmcv`（推荐：CPU 检测正确性 + BMCV 快速绘制）。

### 脚本特性

- **退出码传播**：捕获主程序真实退出码（`set +e; wait $PID; PROCESS_EXIT_CODE=$?; set -e`），禁止 `wait ... || true` 吞码。
- **trap 清理**：EXIT/INT/TERM 时停止主程序和监控子进程、删除 PID 文件、检查残留进程。
- **周期采样**：每 30 秒记录 CSV（timestamp, elapsed_sec, pid_alive, cpu_percent, rss_kb, threads, output_bytes, mem_available_kb, tpu_used_mb）。
- **ffprobe 验证**：输出文件无法读取时返回非 0。
- **残留检查**：结束后 `pgrep -x single_video_infer`，存在残留则强制非 0。

## 短时测试结果

### 10 秒功能测试

| 配置 | 输出帧 | fps | 退出码 | 残留 |
|------|--------|-----|--------|------|
| CPU 预处理 + CPU 绘制 | 49 | ~5 | 0 | 无 |
| BMCV 预处理 + BMCV 绘制 | 99 | ~10 | 0 | 无 |
| BMCV 预处理 + 不绘制 | 99 | ~10 | 0 | 无 |

### 60 秒性能测试

| 指标 | CPU+BMCV（推荐） | BMCV+BMCV（性能） |
|------|-----------------|------------------|
| 输出 fps | 10.02 | 10.03 |
| 推理 fps | 5.00 | 5.00 |
| 预处理均值 | 76ms | 49ms |
| 推理均值 | 15.9ms | 16.3ms |
| 端到端 P95 | 150ms | 122ms |
| 丢帧 | 2 | 1 |
| 退出码 | 0 | 0 |

### 300 秒短时稳定性测试

配置：`--preprocess cpu --draw-mode bmcv`（推荐）

| 验收项 | 结果 | 门禁 |
|--------|------|------|
| 子进程退出码 | 0 | 0 ✅ |
| 实际运行 | 300.0s | ~300s ✅ |
| 输出帧 | 3001 | - |
| 输出 fps | 10.003 | 9.5–10.5 ✅ |
| 推理 fps | 5.003 | ~5 ✅ |
| 推理 P95 | <25ms | <25ms ✅ |
| 端到端 P95 | 150ms | <500ms ✅ |
| 队列 | 0 | ≤1 ✅ |
| RSS 增长 | 356KB/240s | 无持续增长 ✅ |
| 线程数 | 6（恒定） | 无泄漏 ✅ |
| 系统内存 | 3MB 波动 | 稳定 ✅ |
| 崩溃/Core Dump | 无 | 无 ✅ |
| BMRuntime 错误 | 无 | 无 ✅ |
| ffprobe | 通过 | 通过 ✅ |
| 残留进程 | 无 | 无 ✅ |

周期资源采样 CSV（`smoke_5min_periodic.csv`）：

```
elapsed_sec,rss_kb,threads,cpu_percent
30,          25928, 6,      43.7
60,          25940, 6,      44.5
...
270,         26284, 6,      45.0
```

RSS 从 25928KB 到 26284KB，增长 356KB（0.35MB），无持续增长趋势。

## 长时测试（待人工执行）

### 30 分钟

```bash
./tools/video_inference/stability_test.sh 30
```

验收项：完整运行 1800s、退出码 0、ffprobe 成功、RSS 增长 < 50MB、TPU 无持续增长、队列 ≤ 1、延迟无持续增长、无崩溃/Core Dump。

### 2 小时

```bash
./tools/video_inference/stability_test.sh 120
```

验收项：完整运行 7200s、退出码 0、输出文件正常闭合、ffprobe 成功、RSS 增长 < 100MB、TPU 无持续增长、输出 fps 稳定、无持续错误、无残留进程。

> 详细操作指南见 `docs/20-stage4-manual-long-run-guide.md`。

## 历史参考

此前 900 秒稳态观察（旧 CPU 路径，~5fps）记录 RSS 24.7MB 零增长、TPU 75M/550M 稳定、P95 ~391ms 无漂移。此为历史参考，不能替代完整 1800s 退出/封装收尾/资源释放验证，且代码已更新为 BMCV 优化版本。

## 已知限制

1. **BMCV 预处理正确性**：BMCV CSC 与 sws 系数存在差异，检测框 IoU 0.94–0.98、score 差 0.02–0.09，2 帧漏检。CPU 保持默认预处理路径。详见 `docs/19`。
2. **BMCV 无法 resize**：VPP/yuv_resize/resize 在本板均不可用，resize 仍由 libswscale 完成。
3. **TPU 内存报告**：BM1684-SOC 的 `bm-smi` TPU 内存字段显示 N/A，无法精确量化 TPU 内存增长，依赖日志中无 BMRuntime 错误间接验证。
