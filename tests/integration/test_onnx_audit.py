# -*- coding: utf-8 -*-
"""ONNX 审计工具回归测试（中文）。模型缺失时跳过（best.onnx 被 gitignore，仅本地存在）。"""
import os
import sys
import subprocess
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ONNX = os.path.join(REPO, "weights", "best.onnx")


@unittest.skipUnless(os.path.exists(ONNX), "best.onnx 不存在（被 gitignore），跳过审计测试")
class OnnxAuditTest(unittest.TestCase):
    def _run(self):
        return subprocess.run(
            [sys.executable, os.path.join(REPO, "tools", "inspect_onnx.py"), ONNX],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)

    def test_audit_runs_and_key_facts(self):
        r = self._run()
        self.assertEqual(0, r.returncode, msg=r.stdout)
        out = r.stdout
        self.assertIn("images", out)          # 输入名
        self.assertIn("opset_import", out)    # opset 字段
        self.assertIn("ai.onnx=12", out)      # opset 12
        self.assertIn("Conv", out)            # YOLOv7 卷积
        self.assertIn("LeakyRelu", out)       # 激活
        self.assertIn("动态维度", out)         # 动态输入须固化

    def test_input_is_dynamic(self):
        r = self._run()
        self.assertIn("[batch,3,height,width]", r.stdout)  # 确认动态输入形状


if __name__ == "__main__":
    unittest.main()
