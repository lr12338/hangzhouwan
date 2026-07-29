# -*- coding: utf-8 -*-
"""仓库结构完整性门禁（中文日志）。

轻量级检查，不引入新的大型依赖。验证：
1. 无 __pycache__/*.pyc 被跟踪
2. Windows 历史目录不再存在
3. build_release.sh 引用的关键文件存在
4. 无生产密钥/凭据被跟踪
5. .gitignore 规则完整
"""
import os
import subprocess
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _git_ls_files():
    r = subprocess.run(
        ["git", "-C", REPO, "ls-files"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    return [ln for ln in r.stdout.splitlines() if ln]


class RepoStructureTest(unittest.TestCase):
    def test_no_tracked_pycache_or_pyc(self):
        """禁止跟踪 __pycache__ 和 *.pyc 文件。"""
        tracked = _git_ls_files()
        bad = [f for f in tracked if "__pycache__" in f or f.endswith(".pyc")]
        self.assertEqual([], bad, f"发现被跟踪的 Python 缓存文件：{bad}")

    def test_no_windows_historical_dirs(self):
        """Windows 历史目录不得重新进入仓库。"""
        tracked = _git_ls_files()
        beishang = [f for f in tracked if f.startswith("hangzhouwan_beishang/")]
        self.assertEqual([], beishang, f"hangzhouwan_beishang/ 不应被跟踪：{beishang[:5]}")
        nginx = [f for f in tracked if f.startswith("nginx ")]
        self.assertEqual([], nginx, f"nginx 目录不应被跟踪：{nginx[:5]}")

    def test_no_yolov7_requirements(self):
        """yolov7_requirements.txt 已移除，不应重新提交。"""
        tracked = _git_ls_files()
        self.assertNotIn("yolov7_requirements.txt", tracked,
                         "yolov7_requirements.txt 不应被跟踪")

    def test_build_release_referenced_files_exist(self):
        """build_release.sh 引用的关键文件必须存在。"""
        # bmodel 是入库的生产模型
        bmodel = os.path.join(REPO, "artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel")
        self.assertTrue(os.path.isfile(bmodel), f"生产 bmodel 缺失：{bmodel}")

    def test_no_tracked_secrets(self):
        """跟踪文件中不应包含真实生产配置或凭据。"""
        tracked = _git_ls_files()
        forbidden = [f for f in tracked if f in (
            "config/application.yaml",
            "config/logging.yaml",
            ".env",
        )]
        self.assertEqual([], forbidden, f"发现被跟踪的生产配置文件：{forbidden}")

    def test_no_tracked_build_artifacts(self):
        """不应跟踪 build/ 目录或 .o/.a 文件。"""
        tracked = _git_ls_files()
        bad = [f for f in tracked if f.startswith("build/") or f.endswith(".o") or f.endswith(".a")]
        self.assertEqual([], bad, f"发现被跟踪的构建产物：{bad[:5]}")

    def test_gitignore_has_key_patterns(self):
        """ .gitignore 必须包含关键忽略规则。"""
        gi = os.path.join(REPO, ".gitignore")
        with open(gi, encoding="utf-8") as f:
            patterns = f.read()
        required = ["__pycache__/", "*.pyc", "build/", "*.bmodel", "*.pkl", "*.onnx"]
        for p in required:
            self.assertIn(p, patterns, f".gitignore 缺少规则：{p}")


if __name__ == "__main__":
    unittest.main()
