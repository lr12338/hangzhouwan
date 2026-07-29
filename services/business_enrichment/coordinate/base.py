# -*- coding: utf-8 -*-
"""坐标预测统一接口。"""
import abc
import math


class CoordinatePredictor(abc.ABC):
    """坐标预测器抽象接口。

    所有实现必须提供：
      - load()
      - predict(stream_id, detections)
      - health()
      - model_info()
      - close()
    """

    REFERENCE_WIDTH = 2560
    REFERENCE_HEIGHT = 1440

    @abc.abstractmethod
    def load(self):
        """加载模型。仅调用一次。"""

    @abc.abstractmethod
    def predict(self, stream_id, detections):
        """批量预测坐标。

        Args:
            stream_id: "A" 或 "B"
            detections: list of dict, 每个含 x1,y1,x2,y2,image_width,image_height

        Returns:
            list of (lon, lat, valid) tuples
        """

    @abc.abstractmethod
    def health(self):
        """返回健康状态 dict。"""

    @abc.abstractmethod
    def model_info(self):
        """返回模型信息 dict。"""

    @abc.abstractmethod
    def close(self):
        """释放资源。"""

    @staticmethod
    def _validate_output(lon, lat):
        """检查 NaN、Inf 和经纬度范围。"""
        if lon is None or lat is None:
            return 0.0, 0.0, False
        try:
            lon = float(lon)
            lat = float(lat)
        except (TypeError, ValueError):
            return 0.0, 0.0, False
        if math.isnan(lon) or math.isnan(lat):
            return 0.0, 0.0, False
        if math.isinf(lon) or math.isinf(lat):
            return 0.0, 0.0, False
        if not (-180.0 <= lon <= 180.0):
            return 0.0, 0.0, False
        if not (-90.0 <= lat <= 90.0):
            return 0.0, 0.0, False
        return lon, lat, True

    @staticmethod
    def _extract_features_a(x1, y1, x2, y2):
        """A 路特征：[x1, y1, x2, y2]。"""
        return [float(x1), float(y1), float(x2), float(y2)]

    @staticmethod
    def _extract_features_b(x1, y1, x2, y2):
        """B 路特征：[x_center, y_center]。"""
        cx = (float(x1) + float(x2)) / 2.0
        cy = (float(y1) + float(y2)) / 2.0
        return [cx, cy]
