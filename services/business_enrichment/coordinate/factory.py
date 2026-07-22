# -*- coding: utf-8 -*-
"""坐标预测器工厂。"""
from .base import CoordinatePredictor
from .sklearn_predictor import SklearnCoordinatePredictor
from .numpy_forest_predictor import NumpyForestCoordinatePredictor
from .mock_predictor import MockCoordinatePredictor


def create_predictor(mode, model_a_path="", model_b_path="",
                     reference_width=2560, reference_height=1440):
    """根据模式创建坐标预测器。

    Args:
        mode: sklearn | numpy | mock | off
        model_a_path: A 路模型路径
        model_b_path: B 路模型路径

    Returns:
        CoordinatePredictor 实例（off 返回 None）
    """
    if mode == "sklearn":
        predictor = SklearnCoordinatePredictor(
            model_a_path, model_b_path, reference_width, reference_height
        )
    elif mode == "numpy":
        predictor = NumpyForestCoordinatePredictor(
            model_a_path, model_b_path, reference_width, reference_height
        )
    elif mode == "mock":
        predictor = MockCoordinatePredictor(reference_width, reference_height)
    elif mode == "off":
        return None
    else:
        raise ValueError(f"unknown coordinate_mode: {mode}")
    predictor.load()
    return predictor
