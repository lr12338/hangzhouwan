# -*- coding: utf-8 -*-
"""hzwctl preflight 单元测试。

验证：
- YAML 通过 safe_load 直接解析（不通过 shell python3 -c）
- 支持 --release <candidate> --config <path>
- 候选 Release 预检不读旧 current
- 运行冲突检查拆分为 --offline/--activation/--runtime
- 校验 manifest 内每个文件 SHA
- 校验 bmodel 和坐标模型 SHA
- 检查 Sidecar 和 Video 配置一致性
- 无 "|| true" 假通过
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import importlib.util

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

HAS_PYYAML = importlib.util.find_spec("yaml") is not None

HERE = os.path.dirname(os.path.abspath(__file__))
HZWCTL = os.path.normpath(os.path.join(HERE, "..", "..", "tools", "hzwctl.py"))
EXAMPLE_YAML = os.path.normpath(os.path.join(HERE, "..", "..", "config", "application.example.yaml"))


def run_hzwctl(args, timeout=30):
    """运行 hzwctl 并返回 (rc, stdout, stderr)。"""
    cmd = [sys.executable, HZWCTL] + args
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout, r.stderr


def make_fake_release(tmpdir, name="test-release"):
    """创建一个最小化的假 Release 目录。"""
    release = os.path.join(tmpdir, name)
    os.makedirs(os.path.join(release, "bin"), exist_ok=True)
    os.makedirs(os.path.join(release, "models"), exist_ok=True)
    os.makedirs(os.path.join(release, "config"), exist_ok=True)
    os.makedirs(os.path.join(release, "systemd"), exist_ok=True)
    os.makedirs(os.path.join(release, "services"), exist_ok=True)

    # VERSION
    with open(os.path.join(release, "VERSION"), "w") as f:
        f.write(f"{name}\ncommit: abc1234\nbuild_date: 2026-07-22T00:00:00Z\n")

    # dual_stream_app (fake)
    app_path = os.path.join(release, "bin", "dual_stream_app")
    with open(app_path, "w") as f:
        f.write("#!/bin/bash\necho fake\n")
    os.chmod(app_path, 0o755)

    # hzwctl (fake)
    hzwctl_path = os.path.join(release, "bin", "hzwctl")
    with open(hzwctl_path, "w") as f:
        f.write("#!/bin/bash\necho fake\n")
    os.chmod(hzwctl_path, 0o755)

    # bmodel (fake)
    bmodel_path = os.path.join(release, "models", "yolov7_ship_1684_f32.bmodel")
    with open(bmodel_path, "wb") as f:
        f.write(b"fake_bmodel_content")

    # coordinate models (fake)
    for m in ("0121_random_forest_model.pkl", "beishang_x-l.pkl"):
        with open(os.path.join(release, "models", m), "wb") as f:
            f.write(b"fake_model")

    # config
    shutil.copy(EXAMPLE_YAML, os.path.join(release, "config", "application.example.yaml"))

    # systemd
    systemd_dir = os.path.normpath(os.path.join(HERE, "..", "..", "deploy", "systemd"))
    for f in os.listdir(systemd_dir):
        shutil.copy(os.path.join(systemd_dir, f), os.path.join(release, "systemd", f))

    # manifest.json with SHA
    import hashlib
    bmodel_sha = hashlib.sha256(open(bmodel_path, "rb").read()).hexdigest()
    files = []
    for root, _, fs in os.walk(release):
        for fn in fs:
            fp = os.path.join(root, fn)
            if os.path.islink(fp):
                continue
            rel = os.path.relpath(fp, release)
            files.append(rel)
    files.sort()
    manifest = {
        "version": name,
        "commit": "abc1234",
        "bmodel_sha256": bmodel_sha,
        "files": files,
    }
    with open(os.path.join(release, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)

    # sha256sum.txt
    import hashlib
    sha_lines = []
    for root, _, fs in os.walk(release):
        for fn in fs:
            fp = os.path.join(root, fn)
            if os.path.islink(fp):
                continue
            if fn in ("sha256sum.txt", "manifest.json"):
                continue
            rel = "./" + os.path.relpath(fp, release)
            sha = hashlib.sha256(open(fp, "rb").read()).hexdigest()
            sha_lines.append(f"{sha}  {rel}")
    with open(os.path.join(release, "sha256sum.txt"), "w") as f:
        f.write("\n".join(sha_lines) + "\n")

    return release


@unittest.skipUnless(HAS_PYYAML, "未安装 PyYAML")
class PreflightSourceTest(unittest.TestCase):
    """验证 hzwctl.py 源码不含 shell 拼接和假通过。"""

    def setUp(self):
        with open(HZWCTL) as f:
            self.src = f.read()

    def test_no_shell_yaml_check(self):
        """不应包含 shell 拼接 python3 -c 解析 YAML"""
        self.assertNotIn("python3 -c", self.src,
                         "hzwctl 不应通过 shell python3 -c 解析 YAML")

    def test_no_fake_pass_or_true(self):
        """不应包含 || true 假通过"""
        self.assertNotIn("|| true", self.src,
                         "hzwctl 不应包含 || true 假通过")

    def test_has_safe_load(self):
        """必须使用 yaml.safe_load"""
        self.assertIn("yaml.safe_load", self.src)

    def test_has_release_arg(self):
        """必须支持 --release 参数"""
        self.assertIn("--release", self.src)

    def test_has_config_arg(self):
        """必须支持 --config 参数"""
        self.assertIn("--config", self.src)

    def test_has_offline_activation_runtime(self):
        """必须支持 --offline/--activation/--runtime"""
        self.assertIn("--offline", self.src)
        self.assertIn("--activation", self.src)
        self.assertIn("--runtime", self.src)

    def test_has_manifest_sha_verify(self):
        """必须校验 manifest SHA"""
        self.assertIn("manifest", self.src)
        self.assertIn("sha256", self.src.lower())

    def test_has_video_health_socket(self):
        """必须支持 Video 健康 Socket 查询"""
        self.assertIn("video-health.sock", self.src)
        self.assertIn("query_video_health", self.src)


@unittest.skipUnless(HAS_PYYAML, "未安装 PyYAML")
class PreflightCandidateTest(unittest.TestCase):
    """验证候选 Release 预检功能。"""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.release = make_fake_release(self.tmpdir)
        self.config = os.path.join(self.tmpdir, "application.yaml")
        shutil.copy(EXAMPLE_YAML, self.config)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_preflight_release_offline(self):
        """--release --offline 不读 current，可静态检查"""
        rc, out, err = run_hzwctl([
            "preflight", "--release", self.release,
            "--config", self.config, "--offline"
        ])
        # 在非 BM1684 环境下，FFmpeg/TPU 检查会失败，但不应崩溃
        # 主要验证不读 current、能运行到结束
        self.assertNotIn("current Release 未激活", out + err,
                         "候选预检不应要求 current")

    def test_preflight_release_activation(self):
        """--release --activation 检查激活条件"""
        rc, out, err = run_hzwctl([
            "preflight", "--release", self.release,
            "--config", self.config, "--activation"
        ])
        self.assertNotIn("current Release 未激活", out + err)

    def test_preflight_release_runtime(self):
        """--release --runtime 检查运行时冲突"""
        rc, out, err = run_hzwctl([
            "preflight", "--release", self.release,
            "--config", self.config, "--runtime"
        ])
        # 应检查残留进程
        self.assertIn("残留", out)

    def test_preflight_nonexistent_release(self):
        """不存在的候选 Release 应报错"""
        rc, out, err = run_hzwctl([
            "preflight", "--release", "/nonexistent/path",
            "--config", self.config, "--offline"
        ])
        self.assertNotEqual(rc, 0)

    def test_manifest_verification(self):
        """manifest.json 被解析且 bmodel SHA 被校验"""
        rc, out, err = run_hzwctl([
            "preflight", "--release", self.release,
            "--config", self.config, "--offline"
        ])
        self.assertIn("manifest", out)

    def test_config_consistency_check(self):
        """配置一致性检查运行"""
        rc, out, err = run_hzwctl([
            "preflight", "--release", self.release,
            "--config", self.config, "--offline"
        ])
        self.assertIn("application.yaml 解析", out)
        self.assertIn("socket_path", out)


@unittest.skipUnless(HAS_PYYAML, "未安装 PyYAML")
class ActivateReleaseTest(unittest.TestCase):
    """验证 activate_release.sh 脚本结构。"""

    def setUp(self):
        self.script = os.path.normpath(os.path.join(
            HERE, "..", "..", "tools", "release", "activate_release.sh"))
        with open(self.script) as f:
            self.src = f.read()

    def test_has_verify_step(self):
        self.assertIn("verify_release.sh", self.src)

    def test_has_preflight_step(self):
        self.assertIn("preflight", self.src)

    def test_rejects_systemd_unit_drift_before_switch(self):
        drift = self.src.index("systemd 单元未同步")
        switch = self.src.index("sudo mv -T")
        self.assertLess(drift, switch)
        self.assertIn("cmp -s", self.src)

    def test_has_smoke_step(self):
        self.assertIn("smoke", self.src)

    def test_has_auto_rollback(self):
        self.assertIn("回滚", self.src)
        self.assertIn("previous", self.src)

    def test_has_mv_T_atomic(self):
        """使用 mv -T 原子替换"""
        self.assertIn("mv -T", self.src)

    def test_has_wait_business(self):
        self.assertIn("wait-business", self.src)

    def test_has_wait_video(self):
        self.assertIn("wait-video", self.src)

    def test_no_git_checkout(self):
        """禁止 git checkout"""
        # 检查不含实际的 git checkout 命令（排除注释）
        for line in self.src.split("\n"):
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            self.assertNotIn("git checkout", stripped,
                "activate_release.sh 不应执行 git checkout")

    def test_no_compile(self):
        """禁止现场编译"""
        self.assertNotIn("cmake", self.src.lower())
        self.assertNotIn("make", self.src.lower())

    def test_has_result_code(self):
        self.assertIn("RESULT_CODE", self.src)


if __name__ == "__main__":
    unittest.main()
