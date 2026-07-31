import argparse
import contextlib
import importlib.util
import io
import os
import tempfile
import unittest
from unittest import mock


REPO = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
HZWCTL_PATH = os.path.join(REPO, "tools", "hzwctl.py")
SPEC = importlib.util.spec_from_file_location("hzwctl_upstream_test", HZWCTL_PATH)
HZWCTL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HZWCTL)


class UpstreamCheckTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.config = os.path.join(self.temp.name, "application.yaml")
        self.environment = os.path.join(self.temp.name, "video.env")
        with open(self.config, "w", encoding="utf-8") as stream:
            stream.write(
                "streams:\n"
                "  - id: A\n"
                "    enabled: true\n"
                "    input_url_env: A_INPUT\n"
                "    output_url_env: A_OUTPUT\n")
        self.args = argparse.Namespace(
            config=self.config, environment_file=[self.environment],
            allow_degraded=False)

    def tearDown(self):
        self.temp.cleanup()

    def test_environment_files_are_used_without_printing_urls(self):
        with open(self.environment, "w", encoding="utf-8") as stream:
            stream.write(
                "A_INPUT=rtsp://user:secret@example.invalid:554/a\n"
                "A_OUTPUT=rtmp://example.invalid:1935/live/a\n")
        connection = mock.MagicMock()
        connection.__enter__.return_value = connection
        with mock.patch.object(
                HZWCTL.socket, "create_connection",
                return_value=connection) as create, \
                mock.patch.object(HZWCTL.shutil, "which",
                                  return_value="/usr/bin/ffprobe"), \
                mock.patch.object(
                    HZWCTL.subprocess, "run",
                    return_value=mock.Mock(returncode=0)):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(HZWCTL.cmd_upstream_check(self.args), 0)
        self.assertEqual(create.call_count, 2)
        self.assertNotIn("secret", output.getvalue())
        self.assertNotIn("example.invalid", output.getvalue())

    def test_missing_endpoint_fails_closed(self):
        with open(self.environment, "w", encoding="utf-8") as stream:
            stream.write("A_INPUT=\nA_OUTPUT=\n")
        with mock.patch.object(
                HZWCTL.socket, "create_connection") as create:
            self.assertEqual(HZWCTL.cmd_upstream_check(self.args), 75)
        create.assert_not_called()

    def test_allow_degraded_accepts_one_complete_stream(self):
        with open(self.environment, "w", encoding="utf-8") as stream:
            stream.write(
                "A_INPUT=rtsp://user:secret@camera-a/live\n"
                "A_OUTPUT=rtmp://user:secret@media-a/live\n"
                "B_INPUT=rtsp://user:secret@camera-b/live\n"
                "B_OUTPUT=rtmp://user:secret@media-b/live\n")
        with open(self.config, "a", encoding="utf-8") as stream:
            stream.write(
                "  - id: B\n"
                "    enabled: true\n"
                "    input_url_env: B_INPUT\n"
                "    output_url_env: B_OUTPUT\n")
        self.args.allow_degraded = True
        connection = mock.MagicMock()
        connection.__enter__.return_value = connection
        with mock.patch.object(
                HZWCTL.socket, "create_connection",
                return_value=connection), \
                mock.patch.object(HZWCTL.shutil, "which",
                                  return_value="/usr/bin/ffprobe"), \
                mock.patch.object(
                    HZWCTL.subprocess, "run",
                    side_effect=[mock.Mock(returncode=0),
                                 mock.Mock(returncode=1)]):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(HZWCTL.cmd_upstream_check(self.args), 0)
        self.assertIn("运行流: A", output.getvalue())
        self.assertIn("B:RTSP:handshake_failed", output.getvalue())
        self.assertNotIn("secret", output.getvalue())


if __name__ == "__main__":
    unittest.main()
