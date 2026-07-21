# 阶段4 人工长时稳定性测试指南

> 本文件为本地文件输入的人工长时指南；RTSP 输入的人工长时指南见 `22-stage4-2-manual-rtsp-stability.md`。

> 本文档提供 30 分钟和 2 小时人工稳定性测试的完整操作说明。
> Agent 不得自动执行超过 300 秒的测试；以下命令需由人工在板端执行。

## 前置条件

1. 板端路径：`/home/linaro/hangzhouwan-orign/hangzhouwan`
2. 分支：`feat/bm1684-edge-deployment`
3. 已编译：`build/single_video_infer` 存在
4. 测试视频：`testdata/test.mp4`（16.7MB，960×544，20fps，~10s）
5. bmodel：`artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel`

```bash
cd /home/linaro/hangzhouwan-orign/hangzhouwan

# 确认编译最新
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4

# 确认测试基线
python3 tests/run_tests.py
cd build && ctest --output-on-failure && cd ..
```

## 1. 30 分钟稳定性测试

### 1.1 启动

```bash
cd /home/linaro/hangzhouwan-orign/hangzhouwan

./tools/video_inference/stability_test.sh 30
```

默认配置：`--preprocess cpu --draw-mode bmcv`（推荐：CPU 检测正确性 + BMCV 快速绘制）。

也可指定配置：
```bash
./tools/video_inference/stability_test.sh 30 --preprocess cpu --draw-mode bmcv
./tools/video_inference/stability_test.sh 30 --preprocess bmcv --draw-mode bmcv
```

### 1.2 验收项

| 项目 | 要求 |
|---|---|
| 完整运行 | 1800 秒 |
| 子进程退出码 | 0 |
| 输出时长 | 接近 1800 秒（ffprobe duration） |
| ffprobe | 成功读取 |
| RSS 增长 | < 50MB |
| TPU 内存 | 无持续增长 |
| 队列 | 始终 ≤ 1 |
| 端到端延迟 | 无持续单调增长 |
| 崩溃/Core Dump | 无 |
| 线程卡死 | 无 |
| BMRuntime/VPU 错误 | 无持续错误 |
| 残留进程 | 测试结束后无 single_video_infer |

### 1.3 人工检查命令

测试运行中（另开终端）：

```bash
cd /home/linaro/hangzhouwan-orign/hangzhouwan

# 实时查看日志
tail -f artifacts/stage4/logs/medium_30min.log

# 查看进程状态
ps -p "$(cat artifacts/stage4/logs/medium_30min.pid)" \
  -o pid,etime,stat,%cpu,%mem,rss,vsz,cmd

# TPU 状态
bm-smi -noloop

# 系统内存
free -h

# 周期资源采样 CSV
cat artifacts/stage4/logs/medium_30min_periodic.csv
```

测试结束后：

```bash
# ffprobe 验证输出
ffprobe -v error \
  -show_entries format=duration,bit_rate \
  -show_entries stream=codec_name,width,height,r_frame_rate,nb_frames \
  -of default=noprint_wrappers=1 \
  artifacts/stage4/medium_30min.mp4

# 检查残留进程
pgrep -x single_video_infer || echo "无残留"
```

## 2. 2 小时稳定性测试

### 2.1 启动

```bash
cd /home/linaro/hangzhouwan-orign/hangzhouwan

./tools/video_inference/stability_test.sh 120
```

### 2.2 验收项

| 项目 | 要求 |
|---|---|
| 完整运行 | 7200 秒 |
| 子进程退出码 | 0 |
| 输出文件正常闭合 | 是 |
| 输出时长 | 接近 7200 秒 |
| ffprobe | 成功读取 |
| RSS 增长 | < 100MB |
| TPU 内存 | 无持续增长 |
| 输出 fps | 稳定（~10fps） |
| BMRuntime/VPU/FFmpeg 错误 | 无持续错误 |
| 残留进程 | 测试结束后无 single_video_infer |

### 2.3 人工检查命令

测试运行中：

```bash
cd /home/linaro/hangzhouwan-orign/hangzhouwan

# 实时查看日志
tail -f artifacts/stage4/logs/full_2hour.log

# 查看进程状态
ps -p "$(cat artifacts/stage4/logs/full_2hour.pid)" \
  -o pid,etime,stat,%cpu,%mem,rss,vsz,cmd

# TPU 状态
bm-smi -noloop

# 系统内存
free -h

# 周期资源采样 CSV
cat artifacts/stage4/logs/full_2hour_periodic.csv
```

测试结束后：

```bash
# ffprobe 验证输出
ffprobe -v error \
  -show_entries format=duration,bit_rate \
  -show_entries stream=codec_name,width,height,r_frame_rate,nb_frames \
  -of default=noprint_wrappers=1 \
  artifacts/stage4/full_2hour.mp4

# 检查残留进程
pgrep -x single_video_infer || echo "无残留"
```

## 3. 日志和 CSV 交付

测试完成后，将以下文件交给后续 Agent 分析：

```
artifacts/stage4/logs/medium_30min.log          # 30min 运行日志
artifacts/stage4/logs/medium_30min_periodic.csv # 30min 周期资源采样
artifacts/stage4/logs/medium_30min_before.txt   # 30min 测试前快照
artifacts/stage4/logs/medium_30min_after.txt    # 30min 测试后快照

artifacts/stage4/logs/full_2hour.log            # 2h 运行日志
artifacts/stage4/logs/full_2hour_periodic.csv   # 2h 周期资源采样
artifacts/stage4/logs/full_2hour_before.txt     # 2h 测试前快照
artifacts/stage4/logs/full_2hour_after.txt      # 2h 测试后快照
```

### CSV 字段说明

| 字段 | 说明 |
|---|---|
| timestamp | 采样时间 |
| elapsed_sec | 已运行秒数 |
| pid_alive | 主进程是否存活（1/0） |
| cpu_percent | CPU 占用率 |
| rss_kb | RSS 内存（KB） |
| threads | 线程数 |
| output_bytes | 输出文件大小（字节） |
| mem_available_kb | 系统可用内存（KB） |
| tpu_used_mb | TPU 内存使用（MB，若可用） |

### 分析要点

- **RSS**：检查是否持续单调增长（应平稳波动，30min < 50MB，2h < 100MB）
- **TPU**：检查是否持续增长（应平稳）
- **输出字节**：应随时间线性增长
- **队列**：日志中"队列="应始终 ≤ 1
- **延迟**：日志中"P95="应无持续单调增长
- **错误**：日志中不应有持续的"错误"或"BMRT"错误行

## 4. 状态声明

短时 300 秒验证已通过（10fps，退出码 0，RSS 增长 356KB，无残留进程）。

**完整 30 分钟和 2 小时门禁待人工执行。** 在人工执行并提供真实完整运行日志前，不得声明长时稳定性已通过。
