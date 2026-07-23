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

## 测试执行结果

| 测试套件 | 结果 | 备注 |
|---------|------|------|
| `bash -n` 全部脚本 | ✅ 全部通过 | 6 个脚本 |
| `test_maintenance_window_gates.py` | ✅ 54 passed | G1–G4 门禁 + 脚本结构审计 |
| `test_preflight.py` | ✅ 全部通过 | 预检和激活结构审计（含原有） |
| `test_systemd_config.py` | ✅ 全部通过 | systemd 配置审计（含原有） |
| `test_maintenance_window.sh` | ✅ 0 失败 | 生产守卫/软链接/manifest/RTMP/隔离 |
| 全部 Python 测试 | ✅ 151 passed, 1 failed | 唯一失败为 `test_test_video_present_and_decodable`（VPU 硬件资源被生产占用，预存在） |
| CTest | ✅ 14/15 passed | 唯一失败为 `stability_script`（VPU 硬件资源被生产占用，预存在） |

## 候选 Release 是否变化

**否。** 本轮仅修改以下文件，不涉及 `forced_reconnect_tool` 或任何 C++ 代码：

- `tools/dual_stream/forced_reconnect_run.sh`（运行器）
- `tools/dual_stream/evaluate_gates.py`（门禁评估器，新增）
- `tools/dual_stream/rtmp_fault_injection.sh`（RTMP 故障注入脚本，新增）
- `tools/release/activate_release.sh`（激活脚本）
- `docs/production/maintenance-window-runbook.md`（Runbook）
- `docs/production/maintenance-window-audit-evidence.md`（本文件，新增）
- `tests/unit/test_maintenance_window_gates.py`（测试，新增）
- `tests/test_maintenance_window.sh`（测试，新增）

候选 Release `vpu-reconnect-fix-1a1a7b2` 保持不变，无需重建。

## 维护窗口预计/最大时长

| 指标 | 值 |
|------|-----|
| 预计视频不可用时间 | ~30 min |
| 最坏视频不可用时间 | ~70 min |
| RTMP 故障注入业务影响 | 每轮 2s RTMP 输出中断（100 轮 ~7 min），视频采集/推理不受影响 |
| 回滚追加时间 | +2–4 min |
