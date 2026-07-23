# BM1684 视频重连 VPU 显存耗尽：根因与修复

> 状态：修复已合入 `feat/bm1684-edge-deployment`，已构建候选 Release，**本轮未激活、未重启生产**。
> 基线 HEAD：`44f3685`。生产仍在运行旧二进制（与 cleanup `46a151c` 一致），仍存在重连稳定性风险。

## 1. 根因证据表

| 层 | 现象/证据 | 结论 |
|---|---|---|
| 现场实测 | 仅 2 路生产流运行时，VPU 堆(heap2, 2048MB) used≈1937–2031MB(94–99%)，仅余 ~110MB | 远超 2 路正常占用(~300MB)，系历史重连累积泄漏 |
| 现场实测 | `ffprobe` 用 h264_bm 打开解码器失败：`bmvpu_malloc_device_byte_heap failed! VPU_DecOpen failed Error code 0x11` | VPU 堆接近耗尽，新解码器池无法分配(ENOMEM) |
| 最小复现 `repro A_leak` | h264_bm 解码器关闭时仍有 1 帧在途(未 release)：每轮 **+39.5MB**，单调增长 | **主因**：解码器关闭时在途 AVFrame 仍引用 bm_image 池，`avcodec_free_context` 无法回收=>整池(~39.5MB)孤儿化泄漏 |
| 最小复现 `repro B_safe`/FIXED 源 | 先 `av_frame_unref` 归还全部在途帧再关闭解码器：10 次重连 **持平(-6~-12MB)**，0 失败，inflight=0 | 修复路径有效：归零在途帧后关闭解码器不泄漏 |
| 代码审查 `try_reopen_rtsp` | RTSP 重连直接 `close_decoder()`+`avformat_close_input` 后重开，未等待 jitter/decode/encode 队列及处理/编码线程手中的帧释放 | 与复现一致：在途帧未归零即关闭解码器 |
| 代码审查 `reconnect_rtmp` | 普通 RTMP 网络断开执行 `teardown_muxer_encoder()`+`open_encoder()`，每次重建 h264_bm 硬件编码器 | **次因**：网络抖动触发硬件编码器重建，VPU 编码器池 churn/泄漏 |
| 代码审查 `read()` | `avcodec_send_packet` 返回码被忽略；`open2`/`receive_frame` 的 ENOMEM 未升级 | ENOMEM 时仍无限退避重连=>错误风暴，直到 VPU 耗尽 |
| 代码审查 | `extra_frame_buffer_num` 在管线中硬编码为 20 | 不可调，无法按现场调优 |
| 进程退出恢复 | repro 泄漏后进程退出，heap2 由 2016MB 回落至 1937MB(回收 ~79MB) | **致命退出=>进程死亡=>VPU 堆由内核回收**，验证熔断恢复机制 |

> SDK 层排除：`B_safe`(先释放再关闭)显存持平，证明泄漏非 libsophon/sophon-ffmpeg 本身，而是“未释放即关闭”的调用时序错误。本轮**未升级/替换**现场 libsophon/sophon-ffmpeg。

## 2. 修改文件与关键状态机

| 文件 | 关键改动 |
|---|---|
| `include/video/inflight_frame_tracker.h` | 新增：在途 AVFrame 原子计数器(produce/release/wait_drained/overflow)，纯逻辑可单测 |
| `include/video/video_frame.h` | `VideoFrame` 增加 `frame_tracker_`；`release()` 先 `av_frame_unref` 归还 bm_image 池再递减计数器；禁直接 `bm_free_device` |
| `include/video/video_source.h` / `src/video/sophon_ffmpeg_source.cpp` | 新增 `outstanding_avframes()`/`wait_avframes_drained()`/`resource_fatal()`/`reconnect_rtsp()`/`simulate_reconnect()`；`close_decoder()` **未归零禁止关闭**(升级致命)；`read()` 完整检查 send_packet/receive_frame/open2 返回码，ENOMEM=>致命，半初始化上下文立即清理；RTSP 断流返回 `RTSP_RECONNECT` 交管线协调(不再内部重建) |
| `include/video/video_sink.h` / `src/video/sophon_ffmpeg_sink.cpp` | 新增 `decide_rtmp_reconnect()`(纯逻辑决策)+`resource_fatal()`；`teardown_muxer()` 仅释放 muxer/AVIO 保留编码器；`reconnect_rtmp()` **仅重建 muxer/AVIO**；编码器 ENOMEM/未打开=>`DEVICE_RESOURCE_FATAL`；`open_encoder()`/`write()` 返回码全检查+半初始化清理 |
| `include/pipeline/single_stream_pipeline.h` / `src/pipeline/single_stream_pipeline.cpp` | `PipelineConfig.extra_frame_buffer_num`+`rtsp_drain_timeout_ms`；`coordinate_rtsp_reconnect()`：停产生新帧->清空 jitter/decode/encode->等在途帧归零->源重连；`set_resource_fatal()`(退出码 70)；收尾 drain 残留帧保证关闭解码器前 inflight=0；`decode_loop` 重连前排空节流 |
| `include/config/application_config.h` / `src/config/application_config.cpp` | `runtime.extra_frame_buffer_num`(默认 20，校验 >=1) |
| `tools/dual_stream/dual_stream_app.cpp` | `build_pipeline_config` 注入 `extra_frame_buffer_num` |
| `src/application/dual_stream_application.cpp` | 任一路退出码 70=>`request_stop()` 停止双路；`run()` 返回 70 标识资源致命 |
| `tools/dual_stream/forced_reconnect_tool.cpp` / `forced_reconnect_run.sh` | 新增：板端复现/压测工具(heap/repro/decoder/sweep/dual/rtmp)，内置 VPU 显存熔断 |
| `tests/unit_cpp/test_inflight_frame_tracker.cpp` / `test_rtmp_reconnect_decision.cpp` | 新增纯逻辑单测；`test_application_config.cpp` 增 extra_frame_buffer_num 用例 |
| `config/application.example.yaml` | `runtime.extra_frame_buffer_num` 示例 |

**RTSP 重连状态机**：`read()` 断流 -> 返回 `RTSP_RECONNECT` -> `coordinate_rtsp_reconnect`：`rtsp_draining_=true` -> `drain_jitter` + `q_decode.drain` + `q_encode.drain` -> `source.wait_avframes_drained(timeout)` -> (归零) `source.reconnect_rtsp()`(关闭旧解码器[受 inflight==0 保护]->重开输入->重开解码器, epoch+1) -> `rtsp_draining_=false` -> 续读。未归零=>`DEVICE_RESOURCE_FATAL`(70)=>停双路=>非零退出=>systemd 重启=>VPU 堆恢复。

**RTMP 重连状态机**：写失败/断开 -> `decide_rtmp_reconnect` -> 普通网络错误=`REBUILD_MUXER_ONLY`(`teardown_muxer` 保留 enc_ -> 重开 RTMP AVIO -> 重写 header -> PTS reset)；编码器 ENOMEM/未打开=`ESCALATE_FATAL`(70)。

## 3. 测试结果

- 纯逻辑单测(CTest, 无硬件)：`inflight_frame_tracker`/`rtmp_reconnect_decision`/`application_config`/`rtsp_source_options`/`rtmp_backoff`/`latest_frame_queue` 等 **14/14 通过**；pytest 单测 **87 通过**；`bash -n` 全部脚本通过。
- 板端 `repro`(本机,生产在跑,VPU 预算紧张)：A_leak +39.5MB/轮(2 轮后熔断保护生产)；进程退出后 heap2 由 2016MB 回落 1937MB(恢复)。
- 板端 FIXED `decoder 10 8`：10 次重连 ok=10 fail=0 inflight=0，heap2 1943->1931MB(持平)。
- 板端 sweep(5/8/12/16/20, 各 5 次)：5/8/12/20 持平(~-6MB)；buf=16 样本恰逢生产旧二进制自重连泄漏(+39.5MB)叠加，非修复代码回归(已排除)。
- **未在本轮执行**：dual 100x / 完整双路 / RTMP 端到端 100x —— 因生产占用设备且 VPU 堆仅余 ~110MB，继续压测有击穿生产风险。工具(`forced_reconnect_run.sh all`)已就绪，**待维护窗口生产停止后运行**(届时 VPU 预算恢复至 ~2GB)。

## 4. 推荐 extra_frame_buffer_num

**推荐 20**(维持现状)。依据：解码器 bm_image 池基础占用 ~39MB(VPU 内部 dpb，与 extra 关系小)，extra 每档增 ~0.8MB/帧(960x544)。管线最大在途≈jitter5+q_decode1+q_encode1+处理1+编码1≈9 帧，20 提供充足池余量防 bm_image 池死锁与瞬时停滞；sweep 显示 5/8/12/16/20 在修复代码下均稳定。低于 8 在突发解码下余量偏紧，不建议生产调低。

## 5. 新 Release 路径与 manifest

- 构建命令：`bash tools/release/build_release.sh vpu-reconnect-fix`
- 制品路径：`/opt/hangzhouwan/releases/<version>-<commit>/`（含 `bin/dual_stream_app`、`bin/forced_reconnect_tool`、`bin/hzwctl`、`models/`、`systemd/`、`config/`、`VERSION`、`manifest.json`、`sha256sum.txt`）
- manifest 含 version/commit/bmodel_sha256/coord_model sha256/files 清单；`verify_release.sh` 校验 SHA256+manifest。
- **本轮仅构建，不执行 `activate_release.sh`，不切换 `current` 软链接，不重启 `hangzhouwan.target`。**

## 6. 生产维护窗口升级与回滚

**升级(维护窗口)**：
1. 停 `sudo systemctl stop hangzhouwan.target`(释放 VPU 预算，heap2 应回落至基线)。
2. 运行 `bash tools/dual_stream/forced_reconnect_run.sh all`，确认 100x 重连 heap2 持平、进程退出后显存恢复。
3. `bash tools/release/activate_release.sh /opt/hangzhouwan/releases/<version>-<commit>`(预检+冒烟+原子切换 current+重启+60s smoke，失败自动回滚 previous)。
4. 观察 `bm-smi`/健康 Socket；确认 RTSP/RTMP 重连计数正常、heap2 稳定。

**回滚**：`bash tools/release/rollback_release.sh`(current<->previous 软链接切换+重启+readiness 验证，不重新编译、不 git checkout)。

## 7. 约束确认

- 本轮**未激活** cleanup(`46a151c`)、**未修改/未重启**生产服务；生产仍运行旧二进制。
- **未升级/替换**现场 libsophon/sophon-ffmpeg。
- 修复代码仅在 `build/` 与候选 Release 制品中，未进入 `current`。
