# -*- coding: utf-8 -*-
"""维护窗口门禁与脚本安全审计单元测试。

覆盖：
- evaluate_gates.py G1–G4 合成日志门禁评估
- forced_reconnect_run.sh 结构安全审计（无 HZW_FORCE_RUN、唯一 run_id、EXIT trap、
  verify_release 调用、manifest commit 校验、readlink -f 规范化、decoder_reopen_simulation）
- activate_release.sh 结构安全审计（hzwctl 缺失硬失败、systemctl restart 失败硬失败、
  PID 核验、回滚验证、Business+Video 就绪、health 检查、严重错误码 99）
- rtmp_fault_injection.sh 结构安全审计（set -euo pipefail、trap、唯一规则标识、
  目标 IP+1935、journal cursor、A/B 分别计数、前后核对无残留）
- 旧 PASS 汇总隔离测试
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

EVAL_GATE = os.path.join(REPO, "tools", "dual_stream", "evaluate_gates.py")
RUNNER = os.path.join(REPO, "tools", "dual_stream", "forced_reconnect_run.sh")
ACTIVATE = os.path.join(REPO, "tools", "release", "activate_release.sh")
RTMP_FI = os.path.join(REPO, "tools", "dual_stream", "rtmp_fault_injection.sh")


def read_file(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def make_raw_log(path, vpu_lines, done_line, extra_lines=None):
    """创建合成 raw 日志文件。"""
    with open(path, "w", encoding="utf-8") as f:
        for line in vpu_lines:
            f.write(line + "\n")
        if extra_lines:
            for line in extra_lines:
                f.write(line + "\n")
        if done_line:
            f.write(done_line + "\n")


def make_pre_summary(path, phases, overall_rc=0, failure_reason=""):
    """创建 pre-summary JSON。"""
    summary = {
        "run_id": "test-run-id",
        "candidate_release": "/test/release",
        "candidate_manifest_commit": "1a1a7b2",
        "script_commit": "test123",
        "expected_commit": "1a1a7b2",
        "overall_rc": overall_rc,
        "overall": "PASS" if overall_rc == 0 else "FAIL",
        "failure_reason": failure_reason,
        "phases": phases,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(summary, f)
    return summary


def run_eval_gates(log_dir, pre_summary_path, output_path):
    """运行 evaluate_gates.py，返回 (rc, output)。"""
    r = subprocess.run(
        [sys.executable, EVAL_GATE, log_dir, pre_summary_path, output_path],
        capture_output=True, text=True, timeout=30,
    )
    return r.returncode, r.stderr


# ===========================================================================
# evaluate_gates.py 功能测试
# ===========================================================================
class EvaluateGatesTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="hzw_gate_test_")
        self.log_dir = os.path.join(self.tmpdir, "logs")
        os.makedirs(self.log_dir)
        self.pre = os.path.join(self.tmpdir, "pre.json")
        self.out = os.path.join(self.tmpdir, "out.json")

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _run_and_load(self, phases, overall_rc=0, failure_reason="", raw_files=None):
        make_pre_summary(self.pre, phases, overall_rc, failure_reason)
        if raw_files:
            for name, content in raw_files.items():
                path = os.path.join(self.log_dir, f"forced_reconnect_{name}.raw")
                with open(path, "w") as f:
                    f.write(content)
        rc, err = run_eval_gates(self.log_dir, self.pre, self.out)
        self.assertEqual(rc, 0, f"evaluate_gates.py failed: {err}")
        with open(self.out) as f:
            return json.load(f)

    def test_g1_not_passed_no_real_rtsp(self):
        """G1 必须为 NOT_PASSED（无真实 RTSP）。"""
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        result = self._run_and_load(phases)
        g1 = next(g for g in result["gate_results"] if g["gate"] == "G1")
        self.assertEqual(g1["status"], "NOT_PASSED")
        self.assertIn("real RTSP", g1["detail"])

    def test_g1_not_evaluated_no_phases(self):
        """无阶段执行时 G1 为 NOT_EVALUATED。"""
        result = self._run_and_load([])
        g1 = next(g for g in result["gate_results"] if g["gate"] == "G1")
        self.assertEqual(g1["status"], "NOT_EVALUATED")

    def test_g2_pass_inflight_zero(self):
        """G2 inflight=0 通过。"""
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        result = self._run_and_load(phases)
        g2 = next(g for g in result["gate_results"] if g["gate"] == "G2")
        self.assertEqual(g2["status"], "PASS")

    def test_g2_fail_inflight_nonzero(self):
        """G2 inflight>0 失败。"""
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 5}]
        result = self._run_and_load(phases)
        g2 = next(g for g in result["gate_results"] if g["gate"] == "G2")
        self.assertEqual(g2["status"], "FAIL")

    def test_g3_pass_no_growth(self):
        """G3 各堆无增长通过。"""
        vpu_lines = [
            "VPU_HEAP | start | heap0:used=300.0MB/avail=1700.0MB heap1:used=100.0MB/avail=900.0MB heap2:used=200.0MB/avail=1800.0MB",
            "VPU_HEAP | round 10 | heap0:used=300.0MB/avail=1700.0MB heap1:used=100.0MB/avail=900.0MB heap2:used=200.0MB/avail=1800.0MB",
            "VPU_HEAP | final | heap0:used=300.5MB/avail=1699.5MB heap1:used=100.0MB/avail=900.0MB heap2:used=200.0MB/avail=1800.0MB",
        ]
        raw = "\n".join(vpu_lines) + "\ndecoder | done | rounds=100 ok=100 fail=0 | heap2_used 200.0->200.0MB delta=+0.0MB\n"
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        result = self._run_and_load(phases, raw_files={"decoder": raw})
        g3 = next(g for g in result["gate_results"] if g["gate"] == "G3")
        self.assertEqual(g3["status"], "PASS")
        # 验证 vpu_heap_analysis 存在且包含所有堆
        self.assertTrue(len(g3["vpu_heap_analysis"]) > 0)
        heaps = g3["vpu_heap_analysis"][0]["heaps"]
        self.assertTrue(len(heaps) >= 3)  # heap0, heap1, heap2

    def test_g3_fail_growth_above_threshold(self):
        """G3 堆增量超过阈值失败。"""
        vpu_lines = [
            "VPU_HEAP | start | heap2:used=200.0MB/avail=1800.0MB",
            "VPU_HEAP | round 50 | heap2:used=250.0MB/avail=1750.0MB",
            "VPU_HEAP | final | heap2:used=300.0MB/avail=1700.0MB",
        ]
        raw = "\n".join(vpu_lines) + "\ndecoder | done | rounds=100 ok=100 fail=0\n"
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        result = self._run_and_load(phases, raw_files={"decoder": raw})
        g3 = next(g for g in result["gate_results"] if g["gate"] == "G3")
        self.assertEqual(g3["status"], "FAIL")

    def test_g3_fail_monotonic_growth(self):
        """G3 单调增长趋势失败。"""
        vpu_lines = [
            "VPU_HEAP | start | heap2:used=200.0MB/avail=1800.0MB",
            "VPU_HEAP | round 10 | heap2:used=205.0MB/avail=1795.0MB",
            "VPU_HEAP | round 20 | heap2:used=210.0MB/avail=1790.0MB",
            "VPU_HEAP | round 30 | heap2:used=215.0MB/avail=1785.0MB",
            "VPU_HEAP | final | heap2:used=220.0MB/avail=1780.0MB",
        ]
        raw = "\n".join(vpu_lines) + "\ndecoder | done | rounds=100 ok=100 fail=0\n"
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        result = self._run_and_load(phases, raw_files={"decoder": raw})
        g3 = next(g for g in result["gate_results"] if g["gate"] == "G3")
        self.assertEqual(g3["status"], "FAIL")

    def test_g4_pass_no_errors(self):
        """G4 无错误通过。"""
        raw = "VPU_HEAP | start | heap2:used=200.0MB/avail=1800.0MB\ndecoder | done | ok=100 fail=0\n"
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        result = self._run_and_load(phases, raw_files={"decoder": raw})
        g4 = next(g for g in result["gate_results"] if g["gate"] == "G4")
        self.assertEqual(g4["status"], "PASS")

    def test_g4_fail_invalid_free(self):
        """G4 invalid free 失败。"""
        raw = "decoder | round 5 | some output\n*** invalid free detected ***\ndecoder | done | ok=100 fail=0\n"
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        result = self._run_and_load(phases, raw_files={"decoder": raw})
        g4 = next(g for g in result["gate_results"] if g["gate"] == "G4")
        self.assertEqual(g4["status"], "FAIL")

    def test_g4_fail_enomem(self):
        """G4 ENOMEM 失败。"""
        raw = "decoder | round 5 | alloc failed: ENOMEM\ndecoder | done | ok=100 fail=0\n"
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        result = self._run_and_load(phases, raw_files={"decoder": raw})
        g4 = next(g for g in result["gate_results"] if g["gate"] == "G4")
        self.assertEqual(g4["status"], "FAIL")

    def test_g4_fail_bmvidecseqinit(self):
        """G4 BMVidDecSeqInit 失败。"""
        raw = "decoder | round 5 | BMVidDecSeqInit failed with error -1\ndecoder | done | ok=100 fail=0\n"
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        result = self._run_and_load(phases, raw_files={"decoder": raw})
        g4 = next(g for g in result["gate_results"] if g["gate"] == "G4")
        self.assertEqual(g4["status"], "FAIL")

    def test_g4_fail_gmem(self):
        """G4 gmem 错误失败。"""
        raw = "decoder | round 5 | gmem allocation error\ndecoder | done | ok=100 fail=0\n"
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        result = self._run_and_load(phases, raw_files={"decoder": raw})
        g4 = next(g for g in result["gate_results"] if g["gate"] == "G4")
        self.assertEqual(g4["status"], "FAIL")

    def test_gate_failure_sets_overall_rc(self):
        """门禁失败必须使 overall_rc 非零。"""
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        result = self._run_and_load(phases, overall_rc=0)
        # G1 is NOT_PASSED -> overall_rc should become 8
        self.assertNotEqual(result["overall_rc"], 0)
        self.assertEqual(result["overall"], "FAIL")

    def test_gate_results_in_json(self):
        """JSON 必须包含 gate_results，不依赖人工 grep。"""
        phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        result = self._run_and_load(phases)
        self.assertIn("gate_results", result)
        self.assertTrue(len(result["gate_results"]) >= 4)
        gate_names = [g["gate"] for g in result["gate_results"]]
        self.assertIn("G1", gate_names)
        self.assertIn("G2", gate_names)
        self.assertIn("G3", gate_names)
        self.assertIn("G4", gate_names)


# ===========================================================================
# 旧 PASS 汇总隔离测试
# ===========================================================================
class PassSummaryIsolationTest(unittest.TestCase):
    """旧 PASS 汇总不得被本次失败复用。"""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="hzw_iso_test_")
        self.log_dir = os.path.join(self.tmpdir, "logs")
        os.makedirs(self.log_dir)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_old_pass_not_overwritten(self):
        """旧 PASS 汇总文件不被新失败运行覆盖。"""
        # 第一次运行：PASS summary
        old_pre = os.path.join(self.tmpdir, "old_pre.json")
        old_out = os.path.join(self.tmpdir, "old_summary.json")
        old_phases = []
        make_pre_summary(old_pre, old_phases, overall_rc=0, failure_reason="")
        run_eval_gates(self.log_dir, old_pre, old_out)
        with open(old_out) as f:
            old_data = json.load(f)

        # 第二次运行：FAIL summary（不同文件名）
        new_pre = os.path.join(self.tmpdir, "new_pre.json")
        new_out = os.path.join(self.tmpdir, "new_summary.json")
        new_phases = [{"name": "decoder", "ok": 100, "fail": 0, "inflight_end": 0}]
        make_pre_summary(new_pre, new_phases, overall_rc=1, failure_reason="phase_failed")
        run_eval_gates(self.log_dir, new_pre, new_out)
        with open(new_out) as f:
            new_data = json.load(f)

        # 旧文件未被修改
        with open(old_out) as f:
            old_data_after = json.load(f)
        self.assertEqual(old_data, old_data_after)
        self.assertEqual(old_data_after["overall"], "PASS")

        # 新文件为 FAIL
        self.assertEqual(new_data["overall"], "FAIL")


# ===========================================================================
# forced_reconnect_run.sh 结构审计
# ===========================================================================
class ForcedReconnectRunnerStructureTest(unittest.TestCase):
    def setUp(self):
        self.src = read_file(RUNNER)

    def test_no_hzW_force_run_bypass(self):
        """不得保留 HZW_FORCE_RUN 绕过能力。"""
        # 检查不含 HZW_FORCE_RUN 的实际使用（排除注释和说明文字）
        for line in self.src.split("\n"):
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            # 允许在说明文字中提到"已删除"，但不允许作为条件使用
            if "HZW_FORCE_RUN" in stripped and "if" in stripped:
                self.fail(f"HZW_FORCE_RUN 仍作为条件使用: {line}")

    def test_has_unique_run_id(self):
        """必须生成唯一 run_id。"""
        self.assertIn("RUN_ID=", self.src)

    def test_unique_summary_file_per_run(self):
        """每次运行使用唯一 summary 文件。"""
        self.assertIn("forced_reconnect_summary_${RUN_ID}", self.src)

    def test_has_exit_trap(self):
        """必须实现 EXIT trap 安全收口。"""
        self.assertIn("trap do_finalize EXIT", self.src)

    def test_finalize_guard_prevents_recursion(self):
        """finalize 必须有守卫防止递归。"""
        self.assertIn("FINALIZE_DONE", self.src)
        self.assertIn('[ "$FINALIZE_DONE" = 1 ] && return 0', self.src)

    def test_calls_verify_release(self):
        """必须调用 verify_release.sh 验证候选完整性。"""
        self.assertIn("verify_release.sh", self.src)

    def test_validates_manifest_commit(self):
        """必须校验 manifest commit。"""
        self.assertIn("EXPECTED_COMMIT", self.src)
        self.assertIn("manifest_commit", self.src.lower())

    def test_readlink_f_normalization(self):
        """必须使用 readlink -f 规范化路径。"""
        self.assertIn("readlink -f", self.src)

    def test_rejects_symlink_alias(self):
        """必须拒绝软链接别名绕过。"""
        self.assertIn("candidate_symlink_alias", self.src)

    def test_decoder_reopen_simulation_mode(self):
        """rtsp 阶段必须更名为 decoder_reopen_simulation。"""
        self.assertIn("decoder_reopen_simulation", self.src)

    def test_no_rtsp_mode_claim(self):
        """不得保留 rtsp 模式宣称真实 RTSP。"""
        # rtsp 模式应已更名为 decoder_reopen_simulation
        self.assertNotIn('rtsp)" "$(phase_timeout RTSP', self.src)

    def test_failure_reason_recorded(self):
        """必须记录 failure_reason。"""
        self.assertIn("FAILURE_REASON", self.src)

    def test_calls_evaluate_gates(self):
        """必须调用 evaluate_gates.py 自动评估门禁。"""
        self.assertIn("evaluate_gates.py", self.src)

    def test_records_script_commit(self):
        """必须记录脚本自身 commit。"""
        self.assertIn("SCRIPT_COMMIT", self.src)

    def test_records_candidate_manifest_commit(self):
        """必须记录候选 manifest commit。"""
        self.assertIn("CANDIDATE_MANIFEST_COMMIT", self.src)

    def test_all_exit_paths_write_summary(self):
        """所有退出路径都必须通过 EXIT trap 写汇总。"""
        # 生产守卫失败时 exit，由 EXIT trap 写汇总
        self.assertIn("FAILURE_REASON=\"production_still_running\"", self.src)
        # 候选校验失败时 exit，由 EXIT trap 写汇总
        self.assertIn("FAILURE_REASON=\"candidate_", self.src)


# ===========================================================================
# activate_release.sh 结构审计
# ===========================================================================
class ActivateReleaseStructureTest(unittest.TestCase):
    def setUp(self):
        self.src = read_file(ACTIVATE)

    def test_hzwctl_missing_hard_fail(self):
        """hzwctl 缺失必须硬失败，不跳过预检。"""
        # 不应保留旧的"跳过预检（不推荐）"模式
        for line in self.src.split("\n"):
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            self.assertNotIn("跳过预检（不推荐）", stripped,
                "不应保留旧的跳过预检模式")
            self.assertNotIn("hzwctl 不可用，跳过", stripped,
                "不应跳过预检")
        self.assertIn("exit 3", self.src)
        self.assertIn("hzwctl 缺失必须硬失败", self.src)

    def test_systemctl_restart_hard_fail(self):
        """systemctl restart 失败必须硬失败并回滚。"""
        self.assertIn("RESULT_CODE=50", self.src)
        self.assertIn("perform_rollback", self.src)

    def test_upstream_gate_runs_before_release_switch(self):
        """RTSP/RTMP 端点不可达时不得切换 current。"""
        gate = self.src.index("upstream-check")
        switch = self.src.index('sudo mv -T "$CURRENT_NEW" "$CURRENT_LINK"')
        self.assertLess(gate, switch)
        self.assertIn("--environment-file /etc/hangzhouwan/video.env",
                      self.src)

    def test_pid_verification(self):
        """切换后必须核验 PID 对应的可执行文件来自候选。"""
        self.assertIn("/proc/$pid/exe", self.src)
        self.assertIn("PID_EXE_OK", self.src)

    def test_pid_verification_supports_separate_service_account(self):
        """维护用户可只读核验独立 Video 账户的进程映像。"""
        self.assertIn(
            'sudo readlink -f "/proc/$pid/exe"', self.src)

    def test_rollback_verifies_pointing(self):
        """自动回滚必须验证 current/previous 指向。"""
        self.assertIn("cur_now", self.src)
        self.assertIn("prev_now", self.src)

    def test_waits_both_business_and_video(self):
        """回滚后同时等待 Business 和 Video 就绪。"""
        rollback_section = self.src[self.src.index("perform_rollback()"):]
        self.assertIn("wait-business", rollback_section)
        self.assertIn("wait-video", rollback_section)

    def test_health_check_after_rollback(self):
        """回滚后执行 health 检查。"""
        rollback_section = self.src[self.src.index("perform_rollback()"):]
        self.assertIn("health", rollback_section)

    def test_severe_error_code_99(self):
        """自动回滚失败必须输出独立严重错误码 99。"""
        self.assertIn("RESULT_CODE=99", self.src)
        self.assertIn("严重错误", self.src)

    def test_no_skip_preflight(self):
        """不得有跳过预检的逻辑（--no-smoke 跳过 smoke 是允许的）。"""
        for line in self.src.split("\n"):
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            # 允许"跳过 smoke"，不允许"跳过预检"
            if "跳过" in stripped and "smoke" not in stripped and "不跳过" not in stripped:
                self.fail(f"不应有跳过逻辑: {line}")

    def test_result_code_90_for_degraded(self):
        """回滚成功但 health 未达 HEALTHY 输出 90。"""
        self.assertIn("RESULT_CODE=90", self.src)

    def test_perform_rollback_defined_before_use(self):
        """perform_rollback 必须在调用前定义。"""
        define_pos = self.src.index("perform_rollback()")
        # 找到第一个非定义调用
        call_pattern = "perform_rollback\n"
        # 定义在主流程之前
        main_start = self.src.index("echo \"=== 激活 Release")
        self.assertLess(define_pos, main_start,
                        "perform_rollback 必须在主流程之前定义")


# ===========================================================================
# rtmp_fault_injection.sh 结构审计
# ===========================================================================
class RtmpFaultInjectionStructureTest(unittest.TestCase):
    def setUp(self):
        self.src = read_file(RTMP_FI)

    def test_set_euo_pipefail(self):
        """必须 set -euo pipefail。"""
        self.assertIn("set -euo pipefail", self.src)

    def test_has_trap_exit_int_term(self):
        """必须有 trap EXIT/INT/TERM 清理规则。"""
        self.assertIn("trap cleanup_rules EXIT", self.src)
        self.assertIn("INT TERM", self.src)

    def test_unique_rule_identifier(self):
        """必须使用唯一规则标识（comment）。"""
        self.assertIn("RULE_TAG", self.src)
        self.assertIn("comment", self.src)

    def test_target_ip_required(self):
        """必须限定明确目标 IP。"""
        self.assertIn("--target-ip", self.src)
        self.assertIn("TARGET_IP", self.src)

    def test_port_1935_specific(self):
        """必须限定 1935 端口。"""
        self.assertIn("1935", self.src)

    def test_no_choose_one(self):
        """不再保留任选其一的开放式操作。"""
        self.assertNotIn("任选其一", self.src)

    def test_journal_cursor(self):
        """使用 journal cursor 或精确时间统计日志。"""
        self.assertTrue("cursor" in self.src.lower() or "TEST_START_CURSOR" in self.src)

    def test_ab_separate_counting(self):
        """分别记录 A/B 两路重连次数。"""
        self.assertIn("RTMP_RECONNECT_A", self.src)
        self.assertIn("RTMP_RECONNECT_B", self.src)

    def test_residual_check_before(self):
        """执行前核对无残留规则。"""
        self.assertIn("RESIDUAL_BEFORE", self.src)

    def test_residual_check_after(self):
        """执行后核对无残留规则。"""
        self.assertIn("RESIDUAL_AFTER", self.src)

    def test_add_delete_failure_stops(self):
        """添加/删除失败立即停止。"""
        self.assertIn("iptables -A", self.src)
        self.assertIn("iptables -D", self.src)
        # 确保有 exit 1 在 iptables 失败后
        self.assertIn("立即停止", self.src)

    def test_encoder_count_check(self):
        """编码器创建计数检查。"""
        self.assertIn("ENC_NOW_COUNT", self.src)
        self.assertIn("BASE_ENC_COUNT", self.src)

    def test_rounds_expectation_documented(self):
        """明确 100 轮预期每路各 100 次。"""
        self.assertIn("每路各", self.src)


if __name__ == "__main__":
    unittest.main()
