# -*- coding: utf-8 -*-
"""脱敏工具单元测试（中文日志）。覆盖：RTSP 账号密码、RTMP Key、URL Token、
MQTT 密码、普通不敏感 URL、空字符串、非法 URL。"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from tools.redact_secrets import redact, redact_url, redact_rtmp, redact_authorization, redact_env_value


class RedactTest(unittest.TestCase):
    def test_rtsp_user_password(self):
        src = "rtsp://admin:FakePwd123@192.0.2.1:554/streaming/Channels/801"
        out = redact(src)
        self.assertNotIn("FakePwd123", out)
        self.assertNotIn("admin", out)  # userinfo 应被清除
        self.assertIn("192.0.2.1", out)
        self.assertIn("凭据已脱敏", out)

    def test_rtmp_key(self):
        src = "rtmp://example.com:1935/HangZhouBridge/StreamKeyA"
        out = redact(src)
        self.assertNotIn("StreamKeyA", out)
        self.assertIn("流Key已脱敏", out)
        self.assertIn("example.com", out)

    def test_url_token(self):
        src = "https://example.com/api?mmsis=413&usertoken=secrettoken"
        out = redact(src)
        self.assertNotIn("secrettoken", out)
        self.assertIn("usertoken=<已脱敏>", out)

    def test_mqtt_password(self):
        src = 'PASSWORD = "hangzhouwan_lurui"'
        out = redact(src)
        self.assertNotIn("hangzhouwan_lurui", out)
        self.assertIn("已脱敏", out)

    def test_authorization_bearer(self):
        out = redact("Authorization: Bearer abcdef123456")
        self.assertNotIn("abcdef123456", out)
        self.assertIn("令牌已脱敏", out)

    def test_normal_url_not_redacted(self):
        src = "https://example.com/path?id=1&name=ship"
        out = redact(src)
        self.assertEqual(src, out)

    def test_empty_string(self):
        self.assertEqual("", redact(""))

    def test_invalid_url(self):
        out = redact_url("://broken")
        # 不应抛异常，且不泄露任何凭据
        self.assertIsInstance(out, str)

    def test_env_sensitive_value(self):
        self.assertEqual("<敏感环境变量已脱敏>", redact_env_value("AIS_MQTT_PASSWORD", "s3cret"))
        self.assertEqual("INFO", redact_env_value("LOG_LEVEL", "INFO"))

    def test_no_false_positive_on_plain_text(self):
        src = "未检测到船舶，跳过该帧"
        self.assertEqual(src, redact(src))


if __name__ == "__main__":
    unittest.main()
