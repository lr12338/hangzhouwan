# -*- coding: utf-8 -*-
"""systemd 配置单元测试。

验证：
- Business ExecStart 含 --config /etc/hangzhouwan/application.yaml
- Video ExecStartPre 含 hzwctl wait-business
- Video 有 network-online.target 依赖
- RuntimeDirectory 只由 Business 声明，Video 不声明
- Video ExecStart 含 --enable-business 和 --business-socket
"""
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SYSTEMD_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "deploy", "systemd"))


def read_unit(name):
    path = os.path.join(SYSTEMD_DIR, name)
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


class BusinessServiceTest(unittest.TestCase):
    def setUp(self):
        self.content = read_unit("hangzhouwan-business.service")

    def test_execstart_has_config(self):
        """ExecStart 必须含 --config /etc/hangzhouwan/application.yaml"""
        self.assertIn("--config", self.content)
        self.assertIn("/etc/hangzhouwan/application.yaml", self.content)

    def test_execstart_uses_venv_python(self):
        self.assertIn("venv/bin/python3", self.content)

    def test_execstart_uses_module(self):
        self.assertIn("-m services.business_enrichment.app", self.content)

    def test_no_runtime_directory(self):
        """Business 不应声明 RuntimeDirectory（共享目录由 tmpfiles.d 管理）"""
        self.assertNotIn("RuntimeDirectory", self.content)

    def test_has_network_online_target(self):
        self.assertIn("network-online.target", self.content)

    def test_not_enabled_by_default(self):
        """不应有 WantedBy=multi-user.target（不自动 enable）"""
        self.assertNotIn("WantedBy=multi-user.target", self.content)

    def test_python_runtime_isolated(self):
        self.assertIn("PYTHONNOUSERSITE=1", self.content)
        self.assertIn("PYTHONDONTWRITEBYTECODE=1", self.content)

    def test_shared_socket_allows_video_group_connect(self):
        app_path = os.path.normpath(os.path.join(
            HERE, "..", "..", "services", "business_enrichment", "app.py"))
        with open(app_path, encoding="utf-8") as stream:
            app = stream.read()
        self.assertIn("os.chmod(sock_path, 0o660)", app)


class VideoServiceTest(unittest.TestCase):
    def setUp(self):
        self.content = read_unit("hangzhouwan-video.service")

    def test_execstartpre_wait_business(self):
        """ExecStartPre 必须含 hzwctl wait-business"""
        self.assertIn("ExecStartPre", self.content)
        self.assertIn("preflight --release /opt/hangzhouwan/current",
                      self.content)
        self.assertIn("--offline --activation", self.content)
        self.assertIn("wait-business", self.content)
        self.assertIn("--timeout 30", self.content)

    def test_execstart_has_config(self):
        self.assertIn("--config", self.content)
        self.assertIn("/etc/hangzhouwan/application.yaml", self.content)

    def test_execstart_has_enable_business(self):
        self.assertIn("--enable-business", self.content)

    def test_execstart_has_business_socket(self):
        self.assertIn("--business-socket", self.content)
        self.assertIn("/run/hangzhouwan/business.sock", self.content)

    def test_no_runtime_directory(self):
        """Video 不应声明 RuntimeDirectory（由 Business 拥有）"""
        self.assertNotIn("RuntimeDirectory", self.content)

    def test_has_network_online_target(self):
        """Video 需 network-online.target 依赖"""
        self.assertIn("network-online.target", self.content)

    def test_wants_business_service(self):
        """Video 用 Wants 而非 Requires，停止 Business 不连带停止 Video"""
        self.assertIn("Wants=hangzhouwan-business.service", self.content)
        self.assertNotIn("Requires=hangzhouwan-business.service", self.content)

    def test_not_enabled_by_default(self):
        self.assertNotIn("WantedBy=multi-user.target", self.content)

    def test_device_whitelist_is_effective(self):
        self.assertIn("PrivateDevices=false", self.content)
        self.assertIn("DevicePolicy=closed", self.content)
        for device in ("/dev/bm-tpu0", "/dev/bm-vpp", "/dev/bmdev-ctl",
                       "/dev/ion", "/dev/jpu", "/dev/vpu"):
            self.assertIn(f"DeviceAllow={device} rw", self.content)

    def test_preflight_paths_are_writable_inside_sandbox(self):
        for path in ("/run/hangzhouwan", "/data/hangzhouwan/events",
                     "/data/hangzhouwan/monitor"):
            self.assertIn(path, self.content)

    def test_preencode_dump_is_disabled_by_default_and_env_overridable(self):
        default = "Environment=HZW_PREENCODE_DUMP_DIR="
        env_file = "EnvironmentFile=/etc/hangzhouwan/video.env"
        self.assertIn(default, self.content)
        self.assertIn(env_file, self.content)
        self.assertLess(self.content.index(default), self.content.index(env_file))
        self.assertIn("/data/hangzhouwan/monitor", self.content)


class TmpfilesTest(unittest.TestCase):
    def setUp(self):
        here = os.path.dirname(os.path.abspath(__file__))
        self.path = os.path.normpath(os.path.join(here, "..", "..", "deploy", "tmpfiles.d", "hangzhouwan.conf"))

    def test_tmpfiles_config_exists(self):
        """共享 /run/hangzhouwan 由 tmpfiles.d 管理"""
        self.assertTrue(os.path.isfile(self.path), f"缺失 {self.path}")
        with open(self.path, "r", encoding="utf-8") as f:
            content = f.read()
        self.assertIn("/run/hangzhouwan", content)
        self.assertIn("hangzhouwan", content)
        self.assertNotIn(" linaro ", content)


class TargetTest(unittest.TestCase):
    def setUp(self):
        self.content = read_unit("hangzhouwan.target")

    def test_wants_services_without_reverse_stop_propagation(self):
        self.assertNotIn("Requires=", self.content)
        self.assertIn("Wants=", self.content)
        self.assertIn("hangzhouwan-business.service", self.content)
        self.assertIn("hangzhouwan-video.service", self.content)
        self.assertIn("hangzhouwan-supervisor.service", self.content)


class SupervisorServiceTest(unittest.TestCase):
    def setUp(self):
        self.content = read_unit("hangzhouwan-supervisor.service")

    def test_python_module_has_release_working_directory(self):
        self.assertIn(
            "WorkingDirectory=/opt/hangzhouwan/current", self.content)

    def test_uses_shared_runtime_group_without_dac_capability(self):
        self.assertIn("User=root", self.content)
        self.assertIn("Group=hangzhouwan", self.content)
        self.assertIn("CapabilityBoundingSet=", self.content)

    def test_loads_business_and_video_environment(self):
        """上游可达性检查必须能解析 A/B 输入及输出地址。"""
        self.assertIn(
            "EnvironmentFile=/etc/hangzhouwan/business.env", self.content)
        self.assertIn(
            "EnvironmentFile=/etc/hangzhouwan/video.env", self.content)


class MaintenanceRestartTest(unittest.TestCase):
    def setUp(self):
        self.service = read_unit("hangzhouwan-maintenance-restart.service")
        self.timer = read_unit("hangzhouwan-maintenance-restart.timer")

    def test_timer_runs_daily_at_0330(self):
        self.assertIn("OnCalendar=*-*-* 03:30:00", self.timer)
        self.assertIn("Persistent=true", self.timer)
        self.assertIn("WantedBy=timers.target", self.timer)

    def test_python_module_has_release_working_directory(self):
        self.assertIn(
            "WorkingDirectory=/opt/hangzhouwan/current", self.service)

    def test_uses_shared_runtime_group(self):
        self.assertIn("User=root", self.service)
        self.assertIn("Group=hangzhouwan", self.service)

    def test_restart_is_ordered_and_health_gated(self):
        self.assertIn("-m services.monitoring.controlled_maintenance", self.service)
        script_path = os.path.normpath(os.path.join(
            HERE, "..", "..", "services", "monitoring",
            "controlled_maintenance.py"))
        with open(script_path, encoding="utf-8") as stream:
            script = stream.read()
        business = script.index(
            '["systemctl", "restart",\n'
            '                    "hangzhouwan-business.service"]')
        wait_business = script.index('"wait-business"')
        video = script.index(
            '["systemctl", "restart",\n'
            '                    "hangzhouwan-video.service"]')
        wait_video = script.index('"wait-health"')
        self.assertLess(business, wait_business)
        self.assertLess(wait_business, video)
        self.assertLess(video, wait_video)

    def test_timeout_allows_failure_diagnostics_and_cleanup(self):
        self.assertIn("TimeoutStartSec=420", self.service)


class MaintenanceRecoveryTest(unittest.TestCase):
    def setUp(self):
        self.service = read_unit(
            "hangzhouwan-maintenance-recovery.service")
        self.timer = read_unit("hangzhouwan-maintenance-recovery.timer")
        self.target = read_unit("hangzhouwan.target")

    def test_timer_retries_every_thirty_minutes_and_persists(self):
        self.assertIn("OnUnitInactiveSec=30min", self.timer)
        self.assertIn("Persistent=true", self.timer)

    def test_worker_keeps_supervisor_alive(self):
        self.assertIn(
            "-m services.monitoring.maintenance_recovery", self.service)
        self.assertNotIn("hangzhouwan-supervisor.service", self.service)
        self.assertIn("TimeoutStartSec=600", self.service)

    def test_target_starts_recovery_timer(self):
        self.assertIn("hangzhouwan-maintenance-recovery.timer", self.target)
        self.assertIn("Wants=", self.target)
        self.assertNotIn("Requires=", self.target)


class RestartStormPreventionTest(unittest.TestCase):
    """资源致命退出码 70 触发 systemd 恢复，且配置防止高频重启风暴。

    依据：
    - Restart=on-failure：非零退出（含 70）视为失败并重启。
    - Video 仅阻止外部依赖/配置退出码 75/78；70 未被排除，
      故退出码 70 仍触发 on-failure 重启。
    - StartLimitBurst<=5 + StartLimitIntervalSec<=300：限流窗口内最多 5 次重启，
      超限后 systemd 进入 start-limit-hit（failed），需监督器判断，
      避免资源致命（VPU 耗尽）时形成无限高频重启风暴。
    - RestartSec>=5：重启间隔，进一步降低重启频率。
    """

    def setUp(self):
        self.video = read_unit("hangzhouwan-video.service")
        self.business = read_unit("hangzhouwan-business.service")

    def _both(self):
        return (("video", self.video), ("business", self.business))

    def test_both_restart_on_failure(self):
        for name, content in self._both():
            self.assertIn("Restart=on-failure", content,
                          f"{name} 必须 Restart=on-failure 以触发退出码 70 恢复")

    def test_no_success_exit_status(self):
        for name, content in self._both():
            self.assertNotIn("SuccessExitStatus", content,
                             f"{name} 不应声明 SuccessExitStatus，否则 70 可能被误判为成功")

    def test_video_prevents_blind_restart_for_external_or_config_error(self):
        self.assertIn("RestartPreventExitStatus=75 78", self.video)
        self.assertNotIn("RestartPreventExitStatus", self.business)

    def test_start_limit_prevents_storm(self):
        import re
        for name, content in self._both():
            m = re.search(r"StartLimitBurst\s*=\s*(\d+)", content)
            self.assertIsNotNone(m, f"{name} 缺少 StartLimitBurst")
            self.assertLessEqual(int(m.group(1)), 5,
                                 f"{name} StartLimitBurst 过大，无法防重启风暴")
            mi = re.search(r"StartLimitIntervalSec\s*=\s*(\d+)", content)
            self.assertIsNotNone(mi, f"{name} 缺少 StartLimitIntervalSec")
            expected_max = 600 if name == "video" else 300
            self.assertLessEqual(int(mi.group(1)), expected_max,
                                 f"{name} StartLimitIntervalSec 过长")

    def test_restart_sec_spacing(self):
        import re
        for name, content in self._both():
            m = re.search(r"RestartSec\s*=\s*(\d+)", content)
            self.assertIsNotNone(m, f"{name} 缺少 RestartSec")
            self.assertGreaterEqual(int(m.group(1)), 5,
                                    f"{name} RestartSec 过小，重启过快")

    def test_exit_70_triggers_restart(self):
        """综合判定：退出码 70 在当前配置下必然触发 systemd on-failure 重启。"""
        for name, content in self._both():
            self.assertIn("Restart=on-failure", content)
            self.assertNotIn("SuccessExitStatus", content)
            if "RestartPreventExitStatus" in content:
                prevented = content.split(
                    "RestartPreventExitStatus=", 1)[1].splitlines()[0]
                self.assertNotIn("70", prevented.split())


if __name__ == "__main__":
    unittest.main()
