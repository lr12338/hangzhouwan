# -*- coding: utf-8 -*-
"""
杭州湾双路船舶检测 · 上层配置入口（与 utils_demo/config.py 对齐的安全整改版）。

阶段1整改：移除明文凭据与 Windows 路径，改为环境变量 + 仓库相对路径。
算法逻辑未改动。正式配置见 config/application.example.yaml。
"""
import os

from utils_demo.find_ship import (
    beixia_predict_longitude_latitude,
    beishang_predict_longitude_latitude,
)

_BASE_DIR = os.environ.get(
    "HANGZHOUWAN_BASE_DIR",
    os.path.dirname(os.path.abspath(__file__)),
)
_WEIGHTS_DIR = os.path.join(_BASE_DIR, "weights")


def _env(name, default=""):
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

ffmpeg_path = os.environ.get("HANGZHOUWAN_FFMPEG_PATH", "ffmpeg")
font_path = os.path.join(_WEIGHTS_DIR, "simhei.ttf")
detector_path = os.path.join(_WEIGHTS_DIR, "best.onnx")
beixia_model_path = os.path.join(_WEIGHTS_DIR, "0121_random_forest_model.pkl")
beishang_model_path = os.path.join(_WEIGHTS_DIR, "beishang_x-l.pkl")
