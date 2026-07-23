# 阶段4.2：单路 RTSP 输入到本地文件

> 状态：**代码与单元测试完成；RTSP 连接/超时/中断/脱敏路径已用受控无效地址验证；实流解码与中途断线重连待人工用真实摄像头或用户提供的中继执行。** 本阶段不接 RTMP/MQTT/AIS/双路。

## 1. 目标

在阶段4 本地视频管线基础上扩展为单路 RTSP 输入：

1. 支持单路 RTSP 输入（`--source-type rtsp`）；
2. 继续输出本地 MP4/TS 文件；
3. 实现连接超时、读取超时与受控断线重连；
4. 保证输出时间戳单调（源 PTS 回退/重连跳变不影响输出）；
5. 保持 h264_bm 硬解 → 推理 → BMCV 绘框 → h264_bm 硬编 链路不变；
6. RTSP 凭据不入命令行/日志/Git，统一环境变量 + 脱敏。

## 2. RTSP 参数依据（板端 Sophon-FFmpeg 4.1.3 实测）

全部 AVDictionary 参数来自板端真实帮助（`ffmpeg -hide_banner -h demuxer=rtsp`），不照搬新版 FFmpeg 或网络文章：

| 选项 | 类型 | 说明（板端实测） |
|---|---|---|
| `rtsp_transport` | flags | `tcp` / `udp` / `udp_multicast` / `http`；推荐 `tcp`（穿 NAT/防火墙更稳） |
| `stimeout` | int（微秒） | socket TCP I/O 超时，覆盖连接与读取超时；0=不限 |
| `reorder_queue_size` | int | 乱序包缓冲（UDP 用） |
| `buffer_size` | int | 底层协议收发缓冲 |
| `rtsp_flags` | flags | `prefer_tcp` 等；listen 仅 demuxer 侧 |

本阶段使用 `rtsp_transport` 与 `stimeout`。`stimeout` 同时充当连接超时与读取超时（TCP 模式下，无数据到达时 `av_read_frame` 在 `stimeout` 后返回错误，触发重连）。

> 注意：本板 Sophon-FFmpeg 的 RTSP **muxer 不支持 listen（服务端）模式**（`ffmpeg -h muxer=rtsp` 无 `rtsp_flags`），故无法用板端 ffmpeg 自建本地 RTSP 中继做实流自测（见第 7 节）。

## 3. 设计（扩展现有管线，不另起一套）

### 3.1 新增/修改文件

| 文件 | 说明 |
|---|---|
| `include/video/rtsp_source_options.h` / `src/video/rtsp_source_options.cpp` | RTSP 选项、URL 脱敏、环境变量解析、重连退避策略（纯逻辑，可单测） |
| `include/video/video_source.h` / `src/video/sophon_ffmpeg_source.cpp` | 新增 `open_rtsp`、中断回调、`read()` 内受控重连、源 epoch |
| `include/video/video_frame.h` | 新增 `source_epoch` 字段（重连检测） |
| `include/video/video_sink.h` / `src/video/sophon_ffmpeg_sink.cpp` | `OutputPtsSequence`：输出 PTS 严格递增，忽略源 PTS |
| `include/pipeline/detection_snapshot.h` | `SnapshotStore::clear()`：重连后清除过期检测结果 |
| `include/pipeline/single_stream_pipeline.h` / `src/pipeline/single_stream_pipeline.cpp` | 配置项、env 解析、RTSP 打开、RTSP 不节流、epoch 清快照、`request_stop` 转发 |
| `tools/video_inference/single_video_infer.cpp` | 新增 CLI 参数 |

### 3.2 URL 凭据脱敏与安全输入

- 禁止 `--input 'rtsp://user:password@ip/path'`（会进入 shell 历史/进程列表/日志）。
- 统一用 `--input-env HZW_TEST_RTSP_URL`，程序从环境变量读取 URL，URL 不出现在命令行。
- 日志脱敏函数 `redact_url_credentials`：`rtsp://user:password@host` → `rtsp://user:***@host`，保留用户名便于排障，绝不输出密码。
- 异常日志（含 FFmpeg 内部 `[tcp @]` 日志）经验证不泄漏密码（见第 7 节）。
- 提交前执行 `python3 tools/redact_secrets.py --scan .` 与 `git grep -nEi 'rtsp://[^ ]+:[^ ]+@'`。

### 3.3 中断回调（停止信号中断阻塞）

`fmt_->interrupt_callback` 指向 `stop_requested_` 原子标志。`avformat_open_input`（连接）与 `av_read_frame`（读取）期间 FFmpeg 周期性调用该回调；`request_stop()`（SIGINT/SIGTERM/限时）置位后，阻塞调用返回 `AVERROR_EXIT`（"Immediate exit requested"），实现及时退出、不永久阻塞。

### 3.4 受控断线重连（状态机）

`read()` 在 RTSP 模式下，当 `av_read_frame` 失败（断流/读取超时）时进入 `reconnect()`：

```
读取失败/断流
  → 若 stop：返回"停止"
  → 否则循环：
       若 max_reconnect_attempts >= 0 且已尝试次数 >= 上限：返回"重连次数耗尽"
       backoff = ReconnectPolicy.on_failure()   # 指数退避：initial*2^(n-1)，封顶 max
       日志：BACKOFF 第N次 退避Xms（URL已脱敏）
       sleep_interruptible(backoff)             # 50ms 片段轮询 stop
       若 stop：返回"停止"
       try_reopen_rtsp()：关闭输入+解码器，重新 open_input + open_decoder
         成功 → on_success()（重置退避）、++reconnect_count、++source_epoch、返回 true
         失败 → 记录原因（不含 URL），继续退避
```

- 初始连接失败（`open_rtsp`）不进入重连，直接返回错误（退出码 3），避免对坏地址无限重连。
- 重连仅发生在已成功打开后的读取失败，受 `--rtsp-max-reconnect` 约束（-1=无限）。
- bmodel 全程只加载一次（在 `run()` 中，与 source 解耦），重连不重新加载 bmodel。
- 重连成功后 `source_epoch` +1；处理线程检测 epoch 变化即 `snapshot_.clear()`，旧检测结果不粘贴到新连接帧。
- 输出 PTS 由 `OutputPtsSequence` 按输出帧序严格递增分配，完全忽略源 PTS，重连跳变不影响输出单调。

## 4. CLI 参数

```
--source-type file|rtsp          # 默认 file
--input-env <NAME>               # RTSP 必填：输入 URL 环境变量名
--rtsp-transport tcp|udp         # 默认 tcp
--rtsp-stimeout-us 5000000       # socket I/O 超时（微秒），默认 5s
--rtsp-max-reconnect -1          # -1=无限 0=不重连 >0=上限
--rtsp-initial-backoff-ms 1000   # 初始退避
--rtsp-max-backoff-ms 30000      # 退避上限
```

推荐配置沿用阶段4：`--preprocess cpu --draw-mode bmcv`。

## 5. 单元测试（纯逻辑，无需网络/硬件）

`ctest` 共 8 项全部通过，其中 RTSP 相关：

| 测试 | 覆盖 |
|---|---|
| `rtsp_source_options` | URL 脱敏（有/无凭据/异常输入）、env 未设置、非法 transport、退避指数增长/封顶/成功重置、`max_reconnect` 语义 |
| `output_pts` | 输出 PTS 严格递增；源 PTS 回退（重连跳回 0）不影响输出；10 万帧无重复 |
| `detection_snapshot` | `clear()` 后无效且过期（重连清除旧框） |
| `pipeline_timing` | 配置校验含 source_type/RTSP 规则 |
| `latest_frame_queue` | 容量1丢旧/关闭退出（重连后队列无历史帧的底层保证） |

Python 测试 29 项通过（含 `redact_secrets` 的 `--scan` 模式：检测真实凭据泄漏、忽略 env 读取与占位符）。

## 6. 板端验证结果（受控无效地址，≤30s）

无密码无效地址（不可路由 IP，短 `stimeout`，不重连）：

```
HZW_TEST_RTSP_URL='rtsp://192.0.2.1:554/test' ./build/single_video_infer \
  --source-type rtsp --input-env HZW_TEST_RTSP_URL \
  --output artifacts/stage4_2/invalid_rtsp.mp4 \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --decoder h264_bm --encoder h264_bm --rtsp-transport tcp \
  --rtsp-stimeout-us 2000000 --rtsp-max-reconnect 0 \
  --source-fps 20 --output-fps 10 --inference-fps 5 --queue-size 1 \
  --preprocess cpu --draw-mode bmcv --max-seconds 25
# 输出：[tcp @ ...] Connection to tcp://192.0.2.1:554?timeout=2000000 failed: Connection timed out
#       错误 | 视频源 | 打开输入失败: Connection timed out
# 退出码 3，约 2 秒返回，无残留进程
```

伪造凭据无效地址（验证脱敏）：日志与 FFmpeg 内部 `[tcp @]` 行均不含密码 `FAKEPASS123`，仅 `错误 | 视频源 | 打开输入失败: Connection timed out`。

SIGTERM/SIGINT 中断阻塞打开（长 `stimeout`=30s，本会挂起；3s 后发信号）：

```
# SIGTERM：信息 | 信号 | 收到信号 15，请求停止  →  错误 | 视频源 | 打开输入失败: Immediate exit requested  退出码 3
# SIGINT ：信息 | 信号 | 收到信号 2 ，请求停止  →  错误 | 视频源 | 打开输入失败: Immediate exit requested  退出码 3
# 均 <1s 退出，无残留进程
```

20 秒本地文件回归（确认硬件链路未回退）：输出 198 帧 ~10fps，h264_bm 解码/编码，mp4，P95=149ms，队列=0，退出码 0，ffprobe 通过，无残留。

| 验收项 | 结果 |
|---|---|
| 连接超时生效（`stimeout`） | ✅ ~2s 返回 Connection timed out |
| 不永久阻塞 | ✅ `stimeout` 与中断回调双重保证 |
| SIGTERM/SIGINT 可退出阻塞 | ✅ Immediate exit requested，<1s |
| 无残留进程 | ✅ |
| URL/密码脱敏 | ✅ 日志与 FFmpeg 内部日志均不泄漏 |
| 本地文件回归不回退 | ✅ 20s 通过 |

## 7. 实流验证限制（待人工）

板端 **无法自建本地 RTSP 中继**：

- Sophon-FFmpeg 4.1.3 的 RTSP muxer 不支持 listen（服务端）模式；
- 板端无 MediaMTX/rtsp-simple-server/vlc/gstreamer 等可充当 RTSP 服务端的工具；
- 尝试下载 MediaMTX 静态二进制（arm64，28MB）因板端上行带宽过低（~10KB/s）超时失败。

故以下项**待人工用真实摄像头或用户提供的中继执行**（指南见 `docs/22-stage4-2-manual-rtsp-stability.md`）：

- RTSP 实流 h264_bm 解码（复用文件路径同一解码器，已间接验证）；
- 中途断线重连状态机（退避策略已单测；实流断线场景待验证）；
- 60s/300s 实流吞吐与指标。

> 受控重连如无安全测试源，不执行破坏性测试；以状态机单元测试 + 文档保留人工重连命令为准。

## 8. 已知限制

1. `stimeout` 仅对 TCP 生效；UDP 模式下读取超时依赖 `reorder_queue_size` 与重连，建议生产用 TCP。
2. 初始连接失败不重连（直接退出 3），避免对坏地址无限重连；重连仅在已连接后读取失败时触发。
3. 实流解码与中途重连未在板端自动化验证（见第 7 节）。
4. 不接 RTMP/MQTT/AIS/双路（本阶段范围外）。
