# -*- coding: utf-8 -*-
"""资产离线检查（中文日志）。基于真实本地仓库核验模型、坐标模型、字体、
测试视频的存在性与可读性。不连接任何正式服务。"""
import os
import sys
import shutil
import subprocess
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WEIGHTS = os.path.join(REPO, "hangzhouwan_beishang", "weights")
TESTDATA = os.path.join(REPO, "testdata")

REQUIRED_MISSING_MODELS = [
    os.path.join(WEIGHTS, "best.onnx"),
    os.path.join(WEIGHTS, "0121_random_forest_model.pkl"),
    os.path.join(WEIGHTS, "beishang_x-l.pkl"),
]


def _ffprobe():
    for cand in ("ffprobe", "/opt/sophon/sophon-ffmpeg-latest/bin/ffprobe"):
        p = shutil.which(cand)
        if p:
            return p
    return None


class AssetInventoryTest(unittest.TestCase):
    def test_three_models_absent_and_gitignored(self):
        gi = os.path.join(REPO, ".gitignore")
        with open(gi, encoding="utf-8") as f:
            patterns = f.read()
        for m in REQUIRED_MISSING_MODELS:
            self.assertFalse(os.path.exists(m), f"意外发现模型文件：{m}")
            ext = os.path.splitext(m)[1].lstrip(".")
            self.assertIn(f"*.{ext}", patterns, f".gitignore 未忽略 *.{ext}")

    def test_chinese_font_present(self):
        font = os.path.join(WEIGHTS, "simhei.ttf")
        self.assertTrue(os.path.exists(font), "中文字体 simhei.ttf 缺失")
        self.assertGreater(os.path.getsize(font), 100000, "字体文件过小，可能损坏")

    def test_test_video_present_and_decodable(self):
        video = os.path.join(TESTDATA, "test.mp4")
        self.assertTrue(os.path.exists(video), "测试视频 test.mp4 缺失")
        ffprobe = _ffprobe()
        if not ffprobe:
            self.skipTest("未找到 ffprobe，跳过 Sophon-FFmpeg 可读性检查（C++ 侧可用）")
        r = subprocess.run(
            [ffprobe, "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=codec_name,width,height,r_frame_rate",
             "-of", "default=noprint_wrappers=1", video],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30)
        self.assertEqual(0, r.returncode, msg=r.stderr)
        out = r.stdout
        self.assertIn("codec_name=h264", out)
        self.assertIn("width=960", out)
        self.assertIn("height=544", out)

    def test_no_project_test_image(self):
        # 仅 nginx-rtmp-module 自带 bg.jpg，项目无独立“入库”测试图片。
        # 校准帧 testdata/calibration/*.jpg 为 gitignore 的派生资产，不计入。
        r = subprocess.run(
            ["git", "-C", REPO, "ls-files"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        tracked = [ln for ln in r.stdout.splitlines() if ln]
        project_imgs = [
            p for p in tracked
            if p.lower().endswith((".jpg", ".jpeg", ".png")) and "nginx" not in p
        ]
        self.assertEqual([], project_imgs, f"发现入库项目测试图片（应在 fixtures 显式管理）：{project_imgs}")


if __name__ == "__main__":
    unittest.main()
