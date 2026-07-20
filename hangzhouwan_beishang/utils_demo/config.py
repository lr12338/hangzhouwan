# -*- coding: utf-8 -*-
"""
杭州湾双路船舶检测 · 配置入口（旧版兼容层）。

安全整改说明（阶段1）：
- 源码中不再保留任何明文生产凭据、推流地址或令牌；
- RTSP / RTMP 地址一律从环境变量读取，默认为空（禁用）；
- Windows 绝对路径改为仓库相对路径，可由环境变量覆盖；
- 算法逻辑（检测 / 坐标 / AIS 关联）未改动。

正式配置请使用 config/application.yaml（见 config/application.example.yaml）。
"""
import os

from utils_demo.find_ship import (
    beixia_predict_longitude_latitude,
    beishang_predict_longitude_latitude,
)

# 资源根目录：默认为本仓库 hangzhouwan_beishang，可由环境变量覆盖
_BASE_DIR = os.environ.get(
    "HANGZHOUWAN_BASE_DIR",
    os.path.dirname(os.path.abspath(__file__)),
)
_WEIGHTS_DIR = os.path.join(_BASE_DIR, "weights")


def _env(name, default=""):
    """从环境变量读取，默认空字符串，确保未配置时不连接正式服务。"""
    return os.environ.get(name, default)


STREAM_CONFIGS = {
    "A": {
        "stream_id": "A",
        "stream_url": _env("STREAM_A_INPUT_URL"),          # 高清
        "rtmp_url": _env("STREAM_A_OUTPUT_URL"),
        "forbidden_rectangles": [
            ((1480, 0), (2560, 630)),
            ((247, 855), (275, 888)),
            ((2295, 895), (2315, 927)),
        ],
        "forbidden_polygons": [
            [(0, 0), (0, 640), (710, 620), (1260, 620), (1260, 0)],
        ],
        "predict_coord_func": beixia_predict_longitude_latitude,
        "camera_param": 0.5,
    },
    "B": {
        "stream_id": "B",
        "stream_url": _env("STREAM_B_INPUT_URL"),          # 高清
        "rtmp_url": _env("STREAM_B_OUTPUT_URL"),
        "forbidden_rectangles": [
            ((0, 0), (2560, 210)),
        ],
        "forbidden_polygons": [
            [(2160, 210), (2560, 280), (2560, 210)],
        ],
        "predict_coord_func": beishang_predict_longitude_latitude,
        "camera_param": 0.8,
    },
}

# FFmpeg 可执行文件：优先环境变量，回退到 PATH 中的 ffmpeg
ffmpeg_path = os.environ.get("HANGZHOUWAN_FFMPEG_PATH", "ffmpeg")

# 字体与模型路径：仓库相对路径，避免 Windows 绝对路径依赖
font_path = os.path.join(_WEIGHTS_DIR, "simhei.ttf")
detector_path = os.path.join(_WEIGHTS_DIR, "best.onnx")

# 坐标标定模型路径
beixia_model_path = os.path.join(_WEIGHTS_DIR, "0121_random_forest_model.pkl")
beishang_model_path = os.path.join(_WEIGHTS_DIR, "beishang_x-l.pkl")
