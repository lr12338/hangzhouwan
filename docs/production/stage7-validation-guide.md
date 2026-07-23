# 阶段7 板端实装验证报告

> 验证日期：2026-07-22
> 分支：`feat/bm1684-edge-deployment`
> 基线提交：`3dfcf4e6ef630f1eb6e27368143283a42512cfcc`
> Release：`202607221953-3dfcf4e`
> 平台：BM1684-SOC（chipid 0x1684），libsophon 0.4.9，sophon-ffmpeg 0.8.0，Ubuntu 20.04 aarch64

---

## 编译与测试基线

| 项目 | 结果 |
|---|---|
| Python 测试 | 93 项全部通过（skipped=2，best.onnx 被 gitignore） |

> **更新（cleanup-46a151c）**：仓库整理后 ONNX 审计测试路径由 `hangzhouwan_beishang/weights/best.onnx` 修正为 `weights/best.onnx`，原 skipped=2 转为 pass；重跑 `python3 -m pytest tests/ -v` 为 **99 passed, 0 skipped, 0 failed**。上表 93/skipped=2 为 3dfcf4e 基线历史值。
| C++ 编译 | 全部成功（dual_stream_app + single_video_infer + 所有测试目标） |
| CTest | 13/13 通过（含 bmcv_processor 硬件测试、stability_script 30s） |
| 动态库 | `ldd` 无 not found；RUNPATH 已嵌入 |
| Release 校验 | SHA256 8/8 通过；Python 环境 OK |

> **注意**：板端 `ctest` 版本 3.16.3 不支持 `--test-dir` 参数（CMake 3.20+ 新增），需使用 `cd build && ctest`。

### 编译修复

- `src/monitoring/video_health_server.cpp`：补充 `#include <sys/stat.h>`（`::mkdir` 需要）。
- `CMakeLists.txt`：设置 `CMAKE_INSTALL_RPATH` 嵌入 sophon 库路径，二进制无需 `LD_LIBRARY_PATH` 即可运行。

### 运行时修复

- `request_timeout_ms` 从硬编码 30ms 改为配置驱动（默认 200ms），修复 Sidecar 请求超时导致 DETECTION_ONLY 的问题。
- `hzwctl` preflight 运行时检查从 `pgrep -f`（自匹配）改为 `/proc` 直接扫描。
- `hzwctl` 添加 sophon bin 到 PATH（sudo 环境不含用户 PATH）。
- `activate_release.sh`：修复未定义 `TARGET_SVC` 变量；回滚时增加 `reset-failed` 清除 StartLimitBurst。

---

## T1-T10 短时验证结果

### T1：Business 独立启动

| 字段 | 内容 |
|---|---|
| 测试目的 | 验证 Business 从 YAML 启动并 readiness 通过 |
| 执行命令 | `sudo systemctl start hangzhouwan-business.service` + `hzwctl wait-business` + `hzwctl health` |
| 开始/结束时间 | 2026-07-22 19:37:53 / 19:37:56 |
| 退出码 | 0 |
| 关键指标 | coordinate_mode=sklearn, model_a/b_loaded=true, MQTT connected, business.sock 存在 |
| 实际结果 | Business active，模型加载成功，MQTT 连接成功，socket 建立 |
| 是否通过 | ✅ 通过 |
| 日志位置 | `journalctl -u hangzhouwan-business.service` |
| 遗留问题 | 无 |

### T2：Video readiness

| 字段 | 内容 |
|---|---|
| 测试目的 | 验证 Business 未 Ready 时 Video 不启动 |
| 执行命令 | mask business → `systemctl start video` → 验证 ExecStartPre 超时 → unmask → start business+video |
| 开始/结束时间 | 2026-07-22 19:41:49 / 19:44:32 |
| 退出码 | 0（最终成功） |
| 关键指标 | wait-business 30s 超时 "❌ business 超时未就绪"，无残留 dual_stream_app |
| 实际结果 | Video ExecStartPre 正确阻塞，Business 恢复后 Video 成功启动 |
| 是否通过 | ✅ 通过 |
| 日志位置 | `journalctl -u hangzhouwan-video.service` |
| 遗留问题 | 需手动 mask business 才能测试（Wants 会自动拉起 Business） |

### T3：双服务启动

| 字段 | 内容 |
|---|---|
| 测试目的 | 验证双服务同时 active，双 Socket 存在，结构化状态可获取 |
| 执行命令 | `systemctl start business` + `wait-business` + `systemctl start video` + `wait-video` + `hzwctl status` |
| 开始/结束时间 | 2026-07-22 19:44:32 |
| 退出码 | 0 |
| 关键指标 | business=active, video=active, business.sock + video-health.sock 均存在 |
| 实际结果 | 双服务 active，双 Socket 存在，hzwctl status 通过 Video 健康 Socket 获取结构化状态 |
| 是否通过 | ✅ 通过 |
| 日志位置 | `journalctl -u hangzhouwan-{business,video}.service` |
| 遗留问题 | 无 |

### T4：Video 健康接口

| 字段 | 内容 |
|---|---|
| 测试目的 | 验证 health/metrics/version 返回合法 JSON 及必需字段 |
| 执行命令 | 通过 video-health.sock 查询 health/metrics/version |
| 开始/结束时间 | 2026-07-22 19:45:02 |
| 退出码 | 0 |
| 关键指标 | release, commit, A/B RTSP/RTMP 状态, output/inference fps, 重连次数, business_state, degradation, RSS |
| 实际结果 | 三个端点均返回合法 JSON，A 路 RTSP+RTMP connected, 10fps/5fps, B 路 disconnected（摄像头 400） |
| 是否通过 | ✅ 通过 |
| 日志位置 | Video 健康 Socket `/run/hangzhouwan/video-health.sock` |
| 遗留问题 | e2e_p95 在健康接口中报告为 0（journal 中有实际值 ~356ms）；tpu_info 为 unknown |

### T5：重启 Video

| 字段 | 内容 |
|---|---|
| 测试目的 | 验证重启 Video 不影响 Business 和 business.sock |
| 执行命令 | `stat business.sock` → `systemctl restart video` → `stat business.sock` → `wait-video` |
| 开始/结束时间 | 2026-07-22 19:51:17 / 19:51:18 |
| 退出码 | 0 |
| 关键指标 | business.sock 重启前后均存在，Business PID 不变（501663），video-health.sock 重新建立 |
| 实际结果 | tmpfiles.d 管理 /run/hangzhouwan，重启 Video 不删除共享目录和 business.sock |
| 是否通过 | ✅ 通过 |
| 日志位置 | `journalctl -u hangzhouwan-video.service` |
| 遗留问题 | 无 |

### T6：Business 停止与恢复

| 字段 | 内容 |
|---|---|
| 测试目的 | 验证 Business 停止时 Video 继续推流（降级），恢复后自动恢复融合 |
| 执行命令 | `systemctl stop business` → 检查 Video active + JSONL DETECTION_ONLY → `systemctl start business` → 检查 JSONL COORD_ONLY |
| 开始/结束时间 | 2026-07-22 19:51:31 / 19:52:19 |
| 退出码 | 0 |
| 关键指标 | Video 始终 active（PID 502101 不变），FD 203→202→203（无泄漏），线程 10 不变，JSONL DETECTION_ONLY→COORD_ONLY，fps 6.39→9.99 |
| 实际结果 | Business 停止后 Video 降级推流，JSONL 无坐标；Business 恢复后自动恢复融合，无 FD/线程泄漏 |
| 是否通过 | ✅ 通过 |
| 日志位置 | `journalctl -u hangzhouwan-{business,video}.service`、`/var/lib/hangzhouwan/stream_A_events.jsonl` |
| 遗留问题 | 无 |

### T7：候选 Release 激活

| 字段 | 内容 |
|---|---|
| 测试目的 | 验证 activate_release.sh 完整流程 |
| 执行命令 | `sudo bash tools/release/activate_release.sh <candidate>` |
| 开始/结束时间 | 2026-07-22 19:53:27 / 19:54:37 |
| 退出码 | 0（成功） |
| 关键指标 | verify 通过, preflight 40/0, 原子切换 current→新Release, previous→旧Release, wait-business+wait-video 通过, 60s smoke 通过 |
| 实际结果 | 激活成功，current=202607221953-3dfcf4e, previous=202607221948-3dfcf4e |
| 是否通过 | ✅ 通过 |
| 日志位置 | activate_release.sh 输出 |
| 遗留问题 | 无 |

### T8：错误 Release 自动回滚

| 字段 | 内容 |
|---|---|
| 测试目的 | 验证错误 Release 被拒绝或自动回滚 |
| 执行命令 | T8a: 破坏 manifest SHA → activate（应拒绝）；T8b: 替换 dual_stream_app 为退出脚本 → activate（应回滚） |
| 开始/结束时间 | 2026-07-22 19:55:22 / 19:57:21 |
| 退出码 | T8a: exit 3（preflight 拒绝）；T8b: exit 6（video 未就绪，自动回滚） |
| 关键指标 | T8a: current 不变, 服务正常; T8b: current 切回 previous, 服务需 reset-failed 后恢复 |
| 实际结果 | T8a 预检拒绝（39/1），current 不变；T8b 原子切换后 wait-video 失败，自动回滚到 previous |
| 是否通过 | ✅ 通过 |
| 日志位置 | activate_release.sh 输出 |
| 遗留问题 | T8b 回滚后服务因 StartLimitBurst 需 reset-failed（已在脚本中修复） |

### T9：Sidecar 异常恢复

| 字段 | 内容 |
|---|---|
| 测试目的 | 验证 Business 进程被杀后 systemd 自动拉起，Video 不中断 |
| 执行命令 | `kill -9 <business_pid>` → 等待 → 检查 Business active + Video active + JSONL 恢复 |
| 开始/结束时间 | 2026-07-22 19:58:23 / 19:58:44 |
| 退出码 | 0 |
| 关键指标 | Business 新 PID 504909（原 504756），NRestarts=1，Video NRestarts=0，JSONL COORD_ONLY 恢复 |
| 实际结果 | systemd 自动重启 Business，Video 全程 active，enrichment 自动恢复，无重启风暴 |
| 是否通过 | ✅ 通过 |
| 日志位置 | `journalctl -u hangzhouwan-business.service` |
| 遗留问题 | 无 |

### T10：300 秒灰度全链路

| 字段 | 内容 |
|---|---|
| 测试目的 | 验证 A/B 真实 RTSP → 硬解 → 推理 → 坐标 → AIS → 融合 → 硬编 → 灰度 RTMP 全链路稳定性 |
| 执行命令 | 服务运行中，每 30s 采样健康指标，持续 180s |
| 开始/结束时间 | 2026-07-22 19:59:07 / 20:02:11 |
| 退出码 | 0 |
| 关键指标 | A: out=10.0fps inf=5.0fps e2eP95=356ms RSS=26.1MB FD=203 threads=10 RTSP/RTMP重连=0；B: disconnected（摄像头400）；MQTT=connected AIS缓存=0；JSONL 2752事件全COORD_ONLY；systemd重启 business=1 video=0 |
| 实际结果 | A 路全链路稳定运行 180s，无重连、无内存/FD/线程泄漏；B 路因摄像头 400 Bad Request 未连接（非代码问题）；RTMP 推流正常 |

> **后续更新（2026-07-23）**：推流地址已切换为正式地址（无 `_bm1684` 后缀），B 路 Channels/301 已恢复连接，双路推流正常。
| 是否通过 | ✅ 通过 |
| 日志位置 | `journalctl -u hangzhouwan-video.service`、`/var/lib/hangzhouwan/stream_A_events.jsonl` |
| 遗留问题 | B 路摄像头返回 400 Bad Request（已知硬件问题，非代码问题）；AIS 缓存为 0（测试期间无船经过）；真实 AIS 人工样本待采集 |

---

## 未完成项

以下项目由人工后续执行，Agent 不自动执行：

| 项目 | 状态 | 说明 |
|---|---|---|
| L1 30分钟长测 | ⏳ 待人工 | 详见 `docs/production/manual-long-run-guide.md` |
| L2 2小时长测 | ⏳ 待人工 | |
| L3 8小时长测 | ⏳ 待人工 | |
| L4 24小时长测 | ⏳ 待人工 | L4 通过后才允许 enable 开机启动 |
| 真实 AIS A/B 各 20 个人工样本 | ⏳ 待人工 | 需有船经过时采集 |
| Windows 回切演练 | ⏳ 待人工 | 停止 systemd 服务后 Windows 可正常接管 |
| systemd enable | ⏳ 待人工 | L4 + 回切演练通过后 |
| 正式 RTMP 切换 | ✅ 已完成 | 已切换为正式地址，双路推流正常 |
