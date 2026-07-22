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


class VideoServiceTest(unittest.TestCase):
    def setUp(self):
        self.content = read_unit("hangzhouwan-video.service")

    def test_execstartpre_wait_business(self):
        """ExecStartPre 必须含 hzwctl wait-business"""
        self.assertIn("ExecStartPre", self.content)
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
        self.assertIn("linaro", content)


class TargetTest(unittest.TestCase):
    def setUp(self):
        self.content = read_unit("hangzhouwan.target")

    def test_requires_both_services(self):
        self.assertIn("hangzhouwan-business.service", self.content)
        self.assertIn("hangzhouwan-video.service", self.content)


if __name__ == "__main__":
    unittest.main()
