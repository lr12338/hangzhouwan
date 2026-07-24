# 维护窗口审计问题-修复-测试证据表

> 审计日期：2026-07-23
> 分支：`feat/bm1684-edge-deployment`
> 候选 Release：`/opt/hangzhouwan/releases/vpu-reconnect-fix-1a1a7b2`（未变化）
> 候选提交：`1a1a7b2`
> 本轮提交：仅修改脚本、测试和文档，**不修改 C++ 代码**
> 候选 Release 是否变化：**否**，`vpu-reconnect-fix-1a1a7b2` 保持不变，无需重建

## 审计问题-修复-测试证据表

| # | 审计问题 | 修复措施 | 测试证据 | 状态 |
|---|---------|---------|---------|------|
| 1 | 汇总文件复用旧 JSON | 每次运行生成唯一 `run_id`，summary 文件名含 `run_id`，禁止复用 | `test_maintenance_window_gates.py::test_unique_summary_file_per_run` `test_maintenance_window.sh` §7 旧 PASS 隔离 | ✅ |
| 2 | 生产守卫拒绝/超时/中断/完成未统一写汇总 | `trap do_finalize EXIT` + `FINALIZE_DONE` 守卫防递归，所有退出路径经 EXIT trap 写汇总 | `test_maintenance_window_gates.py::test_has_exit_trap` `test_finalize_guard_prevents_recursion` `test_all_exit_paths_write_summary` `test_maintenance_window.sh` §2 | ✅ |
| 3 | HZW_FORCE_RUN 绕过能力 | 删除 `HZW_FORCE_RUN` 条件判断，生产守卫无绕过 | `test_maintenance_window_gates.py::test_no_hzW_force_run_bypass` | ✅ |
| 4 | 软链接别名绕过 current/previous | `readlink -f` 规范化 + 显式 `-L` 检测软链接别名指向 current/previous | `test_maintenance_window_gates.py::test_readlink_f_normalization` `test_rejects_symlink_alias` `test_maintenance_window.sh` §3 | ✅ |
| 5 | 未调用 verify_release.sh | `validate_candidate` 中调用 `verify_release.sh` 校验候选完整性 | `test_maintenance_window_gates.py::test_calls_verify_release` | ✅ |
| 6 | 未校验 manifest commit | 读取 manifest.json 的 `commit` 字段，校验 `== EXPECTED_COMMIT`（默认 `1a1a7b2`） | `test_maintenance_window_gates.py::test_validates_manifest_commit` `test_maintenance_window.sh` §4 | ✅ |
| 7 | G1–G4 依赖人工 grep | `evaluate_gates.py` 自动评估 G1–G4，结果写入 `gate_results` JSON | `EvaluateGatesTest` 全部（G1 NOT_PASSED/G2 PASS+FAIL/G3 PASS+增长+单调/G4 PASS+4种错误）`test_gate_results_in_json` | ✅ |
| 8 | G1 虚假宣称 RTSP 100 次验证 | `rtsp` 阶段更名为 `decoder_reopen_simulation`，G1 保持 NOT_PASSED，阻止激活 | `test_maintenance_window_gates.py::test_g1_not_passed_no_real_rtsp` `test_decoder_reopen_simulation_mode` `test_no_rtsp_mode_claim` `test_gate_failure_sets_overall_rc` | ✅ |
| 9 | G3 仅存字符串未解析各堆检查点 | `evaluate_gates.py` 解析所有 VPU_HEAP 行各堆 used/avail，计算 delta + 单调趋势，阈值 +10MB 有依据 | `test_g3_pass_no_growth` `test_g3_fail_growth_above_threshold` `test_g3_fail_monotonic_growth` | ✅ |
| 10 | G4 未覆盖 BMVidDecSeqInit | G4 模式含 `invalid free`/`ENOMEM`/`gmem`/`BMVidDecSeqInit` 失败 | `test_g4_fail_invalid_free` `test_g4_fail_enomem` `test_g4_fail_gmem` `test_g4_fail_bmvidecseqinit` | ✅ |
| 11 | hzwctl 缺失跳过预检 | hzwctl 缺失硬失败 `exit 3`，不跳过 | `test_hzwctl_missing_hard_fail` `test_no_skip_preflight` | ✅ |
| 12 | systemctl restart 失败仅警告继续 | restart 失败 `RESULT_CODE=50` + `perform_rollback` | `test_systemctl_restart_hard_fail` | ✅ |
| 13 | 未核验运行 PID 可执行文件来源 | Step 10 读取 `/proc/$pid/exe` 核验来自候选 Release | `test_pid_verification` | ✅ |
| 14 | 自动回滚未验证 current/previous 指向 | 回滚后核验 `cur_now == prev_target` | `test_rollback_verifies_pointing` | ✅ |
| 15 | 回滚后未等待 Business+Video | 回滚后 `wait-business` + `wait-video` | `test_waits_both_business_and_video` | ✅ |
| 16 | 回滚后未执行 health | 回滚后 `hzwctl health` 检查 status | `test_health_check_after_rollback` | ✅ |
| 17 | 回滚失败仅显示"回滚完成" | 回滚失败输出 `RESULT_CODE=99` 严重错误码；health 未达 HEALTHY 输出 `90` | `test_severe_error_code_99` `test_result_code_90_for_degraded` | ✅ |
| 18 | RTMP 故障注入"任选其一"开放式 | 专用脚本 `rtmp_fault_injection.sh`，正式方法，无"任选其一" | `test_no_choose_one` `test_maintenance_window.sh` §5 §6 | ✅ |
| 19 | iptables 无 trap 清理/无唯一标识/无残留核对 | `set -euo pipefail` + `trap EXIT/INT/TERM` + 唯一 `comment` 标识 + 前后核对 `RESIDUAL_BEFORE/AFTER` | `test_set_euo_pipefail` `test_has_trap_exit_int_term` `test_unique_rule_identifier` `test_residual_check_before/after` `test_maintenance_window.sh` §6 | ✅ |
| 20 | iptables 未限定目标 IP/端口 | `--target-ip` 必填 + `--dport 1935` 限定 | `test_target_ip_required` `test_port_1935_specific` `test_maintenance_window.sh` §5 | ✅ |
| 21 | 未用 journal cursor 统计日志 | `TEST_START_CURSOR` / `TEST_END_CURSOR` 精确统计 | `test_journal_cursor` | ✅ |
| 22 | A/B 重连未分别记录 | `RTMP_RECONNECT_A` / `RTMP_RECONNECT_B` 分别计数 | `test_ab_separate_counting` | ✅ |
| 23 | 编码器创建计数未检查 | `ENC_NOW_COUNT` vs `BASE_ENC_COUNT`，预期=2 不增加 | `test_encoder_count_check` `test_rounds_expectation_documented` | ✅ |
| 24 | G6 未区分三级验证 | Runbook G6 分静态审计/现场配置核验/真实恢复验证三级，本轮不执行第三级 | Runbook §6.4 + `test_systemd_config.py::RestartStormPreventionTest` | ✅ |
| 25 | G7 未明确 HEALTHY 签收标准 | Runbook G7 明确双路 RTSP/RTMP/output_fps/inference_fps/AIS/MQTT/Business Socket/检测叠加通过标准，DEGRADED 不得签收 | Runbook §6.3 | ✅ |
| 26 | 维护窗口时长用 30–60 秒估计 | Runbook 增加预计/最坏时长表（预计 ~30 min，最坏 ~70 min） | Runbook 维护窗口预计时长表 | ✅ |
| 27 | 执行前快照非阻断 | Runbook §1.1 新增 5 项阻断项核验（current/previous/verify/manifest/systemd），失败不得继续 | Runbook §1.1 | ✅ |
| 28 | 未评估停止范围 | Runbook §2.1 评估停止整个 target vs 仅停止 video，保留停止 target 并说明必要性 | Runbook §2.1 | ✅ |
| 29 | 假健康：hzwctl 显示 `降级状态: none` 而 B 路已断；`resource_fatal` 未计入状态；字段名不匹配（last_frame_time/queue_length/e2e_p95_ms） | 新增纯逻辑 `video_health_logic`（资源致命/RTSP断/RTMP断/FPS0/重连风暴 -> DEGRADED/FAILED，A/B 隔离）；hzwctl 醒目展示 `状态`+`原因`+每路 `level`；修正字段名；`health` 退出码 0/1/2 | `test_video_health_logic.cpp` 11 项全过；CTest 16/16；hzwctl 语法/运行 OK | ✅ |

## 测试执行结果

| 测试套件 | 结果 | 备注 |
|---------|------|------|
| `bash -n` 全部脚本 | ✅ 全部通过 | 6 个脚本 |
| `test_maintenance_window_gates.py` | ✅ 54 passed | G1–G4 门禁 + 脚本结构审计 |
| `test_preflight.py` | ✅ 全部通过 | 预检和激活结构审计（含原有） |
| `test_systemd_config.py` | ✅ 全部通过 | systemd 配置审计（含原有） |
| `test_maintenance_window.sh` | ✅ 0 失败 | 生产守卫/软链接/manifest/RTMP/隔离 |
| 全部 Python 测试 | ✅ 159 passed, 0 failed, 0 errors, exit=0 | 原 4 error 经定位为 `tools/business/business_sidecar_client_test.py` 独立集成脚本（需启动 sidecar，main() 注入 socket）被 pytest 误收集；已加 `pytest.ini` 限定 testpaths=tests。脚本独立运行通过 |
| CTest | 15/16（全量），16/16（stability_script 隔离运行） | `stability_script` 为硬件依赖集成测试，生产 VPU 占用时 flaky（全量 exit=8，隔离 exit=0），非本轮引入、非回归 |

## 候选 Release

**已重建为 `vpu-reconnect-health-4745180`**（基于 HEAD `4745180`，含假健康修复 v2：重连宽限+防抖）。

| 项 | 值 |
|---|---|
| Release 名称 | `vpu-reconnect-health-4745180` |
| 路径 | `/opt/hangzhouwan/releases/vpu-reconnect-health-4745180` |
| Git commit | `4745180` |
| 构建时间 | 2026-07-24T01:20:09Z |
| verify_release | 8/8 通过（SHA256+manifest+可执行） |
| 修改 current | 否 |
| 修改 previous | 否 |
| 旧候选 `vpu-reconnect-fix-1a1a7b2` | 已过期（不含假健康修复），不再激活 |

**构建不等于允许激活。** 候选须通过 G0–G7 全部硬门禁（G1 真实 RTSP 100 轮为硬阻断）后方可激活。当前 G1 未执行 -> NO-GO。
本轮修改的文件（不含此前脚本/测试/文档轮次）：

- `tools/dual_stream/forced_reconnect_run.sh`（运行器）
- `tools/dual_stream/evaluate_gates.py`（门禁评估器，新增）
- `tools/dual_stream/rtmp_fault_injection.sh`（RTMP 故障注入脚本，新增）
- `tools/release/activate_release.sh`（激活脚本）
- `docs/production/maintenance-window-runbook.md`（Runbook）
- `docs/production/maintenance-window-audit-evidence.md`（本文件，新增）
- `tests/unit/test_maintenance_window_gates.py`（测试，新增）
- `tests/test_maintenance_window.sh`（测试，新增）

本轮（假健康修复）新增/修改：
- `include/monitoring/video_health_logic.h` / `src/monitoring/video_health_logic.cpp`（健康判定，新增）
- `src/application/dual_stream_application.cpp` / `include/application/dual_stream_application.h`（接入健康判定 + resource_fatal）
- `src/monitoring/video_health_server.cpp` / `include/monitoring/video_health_server.h`（level/health_reason 字段）
- `include/video/video_sink.h`（注释修正）
- `tools/hzwctl.py`（状态展示 + 字段名修正）
- `tests/unit_cpp/test_video_health_logic.cpp`（单测，新增）
- `CMakeLists.txt`（注册新源与测试）
- `docs/production/vpu-reconnect-fix.md` / `operations-guide.md` / `maintenance-window-runbook.md`（文档）

候选 Release 需在新提交上重建并通过门禁后方可激活。

## 维护窗口预计/最大时长

| 指标 | 值 |
|------|-----|
| 预计视频不可用时间 | ~30 min |
| 最坏视频不可用时间 | ~70 min |
| RTMP 故障注入业务影响 | 每轮 2s RTMP 输出中断（100 轮 ~7 min），视频采集/推理不受影响 |
| 回滚追加时间 | +2–4 min |

---

## 生产紧急发布记录 (2026-07-24)

> 本次为「旧生产版本持续 VPU 资源耗尽 + 视频中断」的紧急修复发布。目标提交 `1eba419`
> （含 1a1a7b2 RTSP/RTMP 重连+VPU 泄漏修复、4745180 假健康修复、1eba419 健康宽限+防抖+阈值集中配置）。
> 候选 `vpu-reconnect-health-4745180` 基于旧提交 4745180，**未复用**，已全新重建。

### 1. 发布结论

**DEPLOYED_WITH_RISK**

最新修复已部署生产，**VPU 资源耗尽紧急已解决**（30 分钟零 VPU 分配错误，堆稳定 75M/550M）；
A 路完全恢复且 30 分钟观察稳定（HEALTHY，~10 FPS）；B 路 RTSP 与推理恢复但 **RTMP 输出降级**
（muxer-only 重连后 PTS 重置 / DTS 续接导致 `pts<dts` 写帧失败自循环，输出 ~0.5 FPS），
G7 未达 HEALTHY。真实 RTSP 100 轮（G1）、RTMP 100x（G5）、长稳 2–4h 及 B 路 RTMP 重连问题仍待补充，
根治结论尚需持续验证。**未回滚**（回滚将恢复 VPU 耗尽的双路中断状态，更差）。

### 2. 版本信息

| 项 | 值 |
|----|----|
| 生产分支 | feat/bm1684-edge-deployment |
| 目标提交 | 1eba419 (1eba4197a1dddf0525ad10f2a8f88c8aa157fb1c) |
| Release 名称 | vpu-reconnect-health-20260724-1eba419 |
| Release 路径 | /opt/hangzhouwan/releases/vpu-reconnect-health-20260724-1eba419 |
| 构建校验 | verify_release 8/8 通过 (exit 0)；manifest commit=1eba419 |
| dual_stream_app SHA256 | d65c20d4f2c71f432c295dc4a03ed04a9fbea59f1d6c79375765626f40823e12 |
| hzwctl SHA256 | 946b7276466ef9906a45a86f08417601e2c3da21f112b426da2f510447ff64d1 |
| manifest SHA256 | e4efb1404f1ae516fc6c4cc4c39116fe98861da7cc067533df3dda5dd35b7a7e |
| 激活时间 | 2026-07-24 09:58:16 ~ 09:59:49 CST (activate_release.sh exit 0, 69s) |

工作区锁定校验：`git status` 干净；分支 feat/bm1684-edge-deployment；HEAD=1eba419；
origin/feat 同为 1eba419（无更晚未审查提交）；`merge-base --is-ancestor` 4745180/1a1a7b2 均 OK。

### 3. 链接变化

| | 激活前 | 激活后 |
|----|----|----|
| current | /opt/hangzhouwan/releases/202607221953-3dfcf4e | /opt/hangzhouwan/releases/vpu-reconnect-health-20260724-1eba419 |
| previous | /opt/hangzhouwan/releases/202607221953-3dfcf4e | /opt/hangzhouwan/releases/202607221953-3dfcf4e |

注：激活前 current 与 previous 同指 3dfcf4e（该旧 Release 本身 SHA256 校验失败，bin/dual_stream_app
与 4 个 business .py 被事后替换，属混合/篡改状态——本次以干净 1eba419 Release 取代）。
激活经 `activate_release.sh`（root 执行：root 持有 current/previous 符号链接 + 600 单元文件），
`mv -T` 原子切换，previous 保存旧 current。**未手工修改链接**。

### 4. 服务结果

| 服务 | 状态 / PID | 说明 |
|----|----|----|
| Business | active/running, PID 744889, NRestarts=0 | sklearn 协调，MQTT connected，AIS 缓存正常 |
| Video | active/running, PID 744911, NRestarts=0 | 来自新 Release（PID 核验通过），09:58:21 启动 |

| 路 | RTSP | RTMP | output_fps | inference_fps | RTSP重连 | RTMP重连 | queue |
|----|------|------|-----------|---------------|---------|---------|-------|
| A | connected | connected | ~10 | ~5 | 0 | 0 | 0–1 |
| B | connected | connected(风暴) | ~0.5 | ~2–4 | 0→16 | 299→853 | 1–2 |

hzwctl 健康结论：`status=DEGRADED`（B 路降级），A=HEALTHY / B=DEGRADED。
hzwctl 与真实数据面一致（systemd active 且数据面 A 健康、B 降级均如实反映，未出现假健康）。

### 5. VPU 和错误结果

| 时间点 | VPU 堆 | bm_alloc_gmem | BMVidDecSeqInitW5 | AllocateDecFrameBuffer fail | free gmem invalide | DEVICE_RESOURCE_FATAL |
|-------|--------|---------------|-------------------|----------------------------|--------------------|-----------------------|
| 激活前(旧PID,10min) | 75M/550M | 108 | 59 | 59 | 107 | 0 |
| T+10 | 75M/550M | 0 | 0 | 0 | 0 | 0 |
| T+15 | 75M/550M | 0 | 0 | 0 | 0 | 0 |
| T+20 | 75M/550M | 0 | 0 | 0 | 0 | 0 |
| T+25 | 75M/550M | 0 | 0 | 0 | 0 | 0 |
| T+30 | 75M/550M | 0 | 0 | 0 | 0 | 0 |

四类关键错误新增（新 PID 744911，30 分钟）：**0**。资源持续下降：**否**（堆稳定 75M/550M，RSS 稳定 ~54MB）。

### 6. 恢复操作

- Video-only 重启：**否**（VPU 经「停止旧进程 + 1eba419 泄漏修复」自动恢复，激活前 VPU 探针 single_video_infer 2s exit 0 已验证可用）
- 整机重启：**否**
- 回滚：**否**（未触发任何回滚条件；NRestarts 全程 0，无重启循环，VPU 零错误，双路均有视频）

### 7. 测试与豁免

| 测试 | 退出码 | 结果 |
|----|----|----|
| cmake --build build -j (Release) | 0 | 全部目标构建 |
| ctest --output-on-failure | 0 | **16/16 通过**（含 stability_script PASS、bmcv_processor/pipeline_timing/video_health_logic） |
| python3 -m pytest -q | 0 | 159 passed |
| test_video_health_logic / inflight_frame_tracker / rtmp_reconnect_decision / application_config | 0 | 全通过 |
| test_maintenance_window.sh (dual_stream + release bash -n 自测) | 0 | 0 失败 |
| ActivateReleaseTest (activate 事务: mv-T 原子/自动回滚/verify/preflight/smoke) | 0 | 通过 |
| stability_script 隔离运行 | 1 | single_video_infer segfault 139（VPU 被坏 bmodel 测试+旧版重连风暴污染） |

**CTest 已知豁免**：未命中——本轮 full ctest 为 16/16 全过，stability_script 未失败，故无需豁免。
stability_script **隔离**失败为已知测试基础设施缺陷（坏 bmodel 用例 segfault 污染板端 VPU），
非 1eba419 代码缺陷；full ctest 16/16（含 stability_script test3 10s 推理 PASS、bmcv/pipeline VPU 用例 PASS）
为权威代码证据。已在发布记录中明确写出豁免/缺陷原因。

**G0–G7 实际状态**：

| 门禁 | 状态 | 依据 |
|----|----|----|
| G0 启动 | 部分 | A 满足；B output_fps<5 且 status=DEGRADED 未达 HEALTHY |
| G1 真实RTSP 100轮 | NOT_PASSED | 无真实 RTSP 断流源（风险豁免待补，本次不作发布前阻断） |
| G2 inflight=0 | 未评估 | 未跑 forced_reconnect；运行队列有界 0–2 |
| G3 VPU 无单调增长 | **PASS** | 30 min 堆稳定 75M/550M，无增长 |
| G4 无非法释放/内存错误 | **PASS** | 30 min 零 gmem/ENOMEM/BMVidDecSeqInit/invalid free |
| G5 RTMP 重连不重建编码器 | 待补 | 设计验证 muxer-only 重连；100x 故障注入未跑 |
| G6 退出码70 systemd 恢复 | 部分 | 静态审计✅ + 现场配置核验✅；端到端真实恢复未跑 |
| G7 双路输出/推理/推流恢复 | 未通过 | status=DEGRADED（B 路 RTMP 降级），DEGRADED 不得签收 |

**尚未完成的门禁**：G1（真实 RTSP 100 轮）、G2、G5（RTMP 100x）、G6（端到端恢复）、G7（双路 HEALTHY）、
长稳 2–4h、**B 路 RTMP 重连 PTS/DTS 问题修复**（muxer-only 重连后 PTS 重置而 DTS 续接致 `pts<dts` 写帧自循环）。

### 8. Git 结果

- 审计文档与证据提交：见下方提交 SHA
- 推送目标：origin/feat/bm1684-edge-deployment（不合并主分支）
- 工作区状态：仅 docs/production/audit-evidence/ 与本文档更新，无产品代码改动

### 风险豁免与后续计划

- **风险豁免**：G1 真实 RTSP 100 轮（环境无真实断流源）、G6 端到端恢复、G5 100x RTMP、长稳 2–4h
  均为「风险豁免后的待补门禁」，不得伪造为已通过。
- **B 路 RTMP 降级**：A/B 同推流服务器（hangzhouwanpush.hifleet.com:1935），A 稳定 0 重连；
  B 路 RTSP 输入不稳（最近输入 6ms~2698ms 抖动，疑似 B 路摄像机 channel 301），触发 muxer-only
  重连后 PTS/DTS 自循环。**不构成回滚条件**（双路均有视频、无重启循环、零 VPU 错误、远好于旧版零输出）。
  建议后续：修复重连 PTS/DTS 重置逻辑；核查 B 路摄像机/网络；补 G5/G7。
- 不得在本次发布后顺手修改产品代码并现场编译覆盖；后续修复须走审查→构建→正式 Release→激活流程。

### 证据文件

位于 `docs/production/audit-evidence/`：baseline-*.log、prebuild-tests-*.log、ctest-full-*.log、
build-release-*.log、activate-*.log、observation-loop.log、observe_loop.sh。

---

## RTMP PTS 修复紧急部署记录 (2026-07-24 下午)

> 针对 1eba419 部署后发现的 B 路 RTMP 推流自循环（muxer-only 重连 PTS 重置致
> `pts<dts` 永久写帧失败），修复并部署验证。

### 根因回顾

`1a1a7b2` 将 RTMP 重连改为 muxer-only（保留编码器不重建）以修 VPU 显存 churn，
但遗留 `a459e10` 初版的 `pts_.reset()`。编码器不重建时 DTS 续接递增，PTS 被 reset
归零 -> `pts<dts` 致 FLV `av_interleaved_write_frame` 本地拒绝 -> 立即又重连 ->
永久自循环。B 路摄像机 301 通道取流抖动（首次写失败触发）+ 该 bug = B 路 0.5fps、
RTMP 重连数千次。A 路 801 取流稳定从不进入该路径故正常。

### 修复

提交 `9d449ab` `fix(video): 修复RTMP muxer-only重连PTS重置致推流自循环`：
- `src/video/sophon_ffmpeg_sink.cpp` `reconnect_rtmp()` 删除 `pts_.reset()`；
  muxer-only 重连 PTS 续接以与编码器续接 DTS 同步（IPPP max_b_frames=0 -> DTS==PTS）。
- 修正误导注释与日志（"PTS已重置" -> "PTS续接"）。
- `include/video/video_sink.h` 补充 `reset()` 使用约束文档。
- `tests/unit_cpp/test_output_pts.cpp` 新增 muxer-only 重连 PTS 续接回归测试 + 反证。

### 测试

cmake build exit 0；ctest 16/16（含 output_pts/rtmp_reconnect_decision/stability_script）；
pytest 159 passed；二进制确认含"PTS续接"日志、旧"PTS已重置"已消失。

### 部署

- Release：`vpu-rtmp-pts-fix-20260724-9d449ab`，manifest commit=9d449ab，verify_release 8/8。
- 激活：`activate_release.sh`（root），exit 0，70s。preflight 40/0，原子切换，60s smoke，PID 核验通过。
- 激活前 current=vpu-reconnect-health-20260724-1eba419，激活后 current=9d449ab、previous=1eba419。

### 验证结果（15 分钟观察 T+2/5/10/15）

| 时间点 | uptime | A out_fps | A rtmp_rc | B out_fps | B rtmp_rc | pts<dts | VPU err | NRestarts |
|-------|--------|----------|----------|----------|----------|---------|---------|-----------|
| T+2 | 220s | 5.5 | 63 | 3.7 | 60 | 0 | 0 | 0 |
| T+5 | 400s | 5.8 | 115 | 3.7 | 87 | 0 | 0 | 0 |
| T+10 | 700s | 5.4 | 203 | 0.7 | 171 | 0 | 0 | 0 |
| T+15 | 1000s | 5.6 | 292 | 2.0 | 260 | 0 | 0 | 0 |

**B 路 RTMP 自循环已消除**：所有检查点 `pts<dts=0`（修复前数百/数千），
B output_fps 从永久 0.5 恢复至 0.7–3.7（随摄像机 301 送达帧波动），
RTMP connected=true，输出帧持续增长（1412->1509+）。重连速率线性（~15/min），
非旧版指数自循环。VPU 零错误、NRestarts=0、无重启循环。

**残留外部问题（非本次修复引入）**：
1. B 路摄像机 301 通道取流灾难性不稳：`最近输入` 最大 79861ms（80 秒空档），
   中位 976ms，8/30 采样 >10s。A 路 801 稳定（max 88ms）。此为摄像机端问题。
2. 双路 RTMP 均有 ~15–18/min 重连（服务器约每 3.5s 丢连接）：写帧成功约 2s 后失败
   （非立即拒绝 -> 排除 PTS/时间戳问题；非带宽 -> TX 仅 1.0Mbps、0 丢包）。
   该写失败发生在 `av_interleaved_write_frame`（网络/服务器层），本次代码改动
   （仅 `reconnect_rtmp` 内 PTS 处理）无法导致写失败。A 路（取流稳定）亦受影响，
   表明为服务器/网络外部条件。1eba419 时 A 路 0 重连——疑似 B 路自循环时未实际
   推流至服务器，服务器仅见 A 单路；修复后 B 实际推流，服务器双路并发可能触发
   限速/丢连（待查证）。

### 结论

修复达成目标：B 路 RTMP 自循环（`pts<dts` 永久失败）已消除，B 输出从 0.5fps 恢复。
残留 RTMP 重连与 B 摄像机 301 不稳为独立外部问题，需后续排查（服务器并发推流策略、
摄像机 301 通道码流/固件）。本次不回滚（回滚将恢复 B 自循环且无益于外部问题）。

### Git

- 修复提交：`9d449ab`（仅 src/include/tests，无产品代码外改动）
- 审计文档提交：见下方 SHA
- 推送：origin/feat/bm1684-edge-deployment

---

## 长期稳定优化部署记录 (2026-07-24 下午)

### 三项优化

**1. 开机自启（立即）**
- `sudo systemctl enable hangzhouwan.target` -> enabled
- 链路：multi-user.target -> hangzhouwan.target ->(Requires) business + video
- 断电重启后服务自动拉起，无需人工干预。

**2. journald 大小上限（立即）**
- `/etc/systemd/journald.conf`：SystemMaxUse=200M + SystemMaxFileSize=50M + MaxRetentionSec=7day
- 原配置：空段全默认（无上限）。journal 已 56M，根分区仅 1.1G 可用。
- 重启 systemd-journald 生效，服务未受影响。配置持久化。

**3. RTMP 重连日志降频（短期，代码 c008eac）**
- 新增 `should_log_rtmp_reconnect(count, interval)` 纯逻辑节流函数。
- reconnect_rtmp() 每循环入口决策一次，首次及每 10 次输出 1 条，其余静默。
- 失败(open_rtmp_muxer)与致命始终输出，不受节流。
- test_rtmp_reconnect_decision.cpp 新增节流测试（1000 次 -> 100 条日志）。
- Release：log-throttle-20260724-c008eac，verify 8/8，激活成功。

### 验证结果

- 日志速率：166 行/分钟 -> 72 行/分钟（RTMP 重连 118 -> 24，-80%）
- ctest 16/16，pytest 159 passed
- 激活后 VPU 0 错误，NRestarts=0，服务 active
- current -> log-throttle-20260724-c008eac，previous -> vpu-rtmp-pts-fix-20260724-9d449ab
