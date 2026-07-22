# -*- coding: utf-8 -*-
"""Mock 坐标预测器（仅限 development 环境）。"""
from .base import CoordinatePredictor


class MockCoordinatePredictor(CoordinatePredictor):
    """生成确定性模拟坐标，仅用于开发测试。

    在 production 环境中被 BusinessConfig 禁止。
    """

    BASE_LON = 121.035
    BASE_LAT = 30.560

    def __init__(self, reference_width=2560, reference_height=1440):
        self.REFERENCE_WIDTH = reference_width
        self.REFERENCE_HEIGHT = reference_height
        self._loaded = False
        self._predict_count = 0

    def load(self):
        self._loaded = True

    def predict(self, stream_id, detections):
        results = []
        for det in detections:
            x1, y1, x2, y2 = det["x1"], det["y1"], det["x2"], det["y2"]
            iw = det.get("image_width", self.REFERENCE_WIDTH)
            ih = det.get("image_height", self.REFERENCE_HEIGHT)
            cx = (x1 + x2) / 2.0 / max(iw, 1)
            cy = (y1 + y2) / 2.0 / max(ih, 1)
            lon = self.BASE_LON + (cx - 0.5) * 0.02
            lat = self.BASE_LAT - cy * 0.01
            lon, lat, valid = self._validate_output(lon, lat)
            results.append((lon, lat, valid))
        self._predict_count += len(results)
        return results

    def health(self):
        return {
            "loaded": self._loaded,
            "mode": "mock",
            "predict_count": self._predict_count,
            "error_count": 0,
        }

    def model_info(self):
        return {"loaded": self._loaded, "mode": "mock"}

    def close(self):
        self._loaded = False
