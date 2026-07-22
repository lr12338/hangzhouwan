# -*- coding: utf-8 -*-
"""Sklearn 原生坐标预测器。"""
import os
import time

import numpy as np

from .base import CoordinatePredictor


class SklearnCoordinatePredictor(CoordinatePredictor):
    """使用 sklearn 原生 RandomForestRegressor.predict 进行坐标预测。"""

    def __init__(self, model_a_path, model_b_path,
                 reference_width=2560, reference_height=1440):
        self.model_a_path = model_a_path
        self.model_b_path = model_b_path
        self.REFERENCE_WIDTH = reference_width
        self.REFERENCE_HEIGHT = reference_height
        self.rf_a = None
        self.rf_b = None
        self._loaded = False
        self._load_time_ms = 0.0
        self._predict_count = 0
        self._error_count = 0

    def load(self):
        import joblib
        t0 = time.time()
        self.rf_a = joblib.load(self.model_a_path)
        self.rf_b = joblib.load(self.model_b_path)
        self._load_time_ms = round((time.time() - t0) * 1000, 2)
        self._loaded = True

    def predict(self, stream_id, detections):
        if not self._loaded:
            raise RuntimeError("predictor not loaded")
        results = []
        model = self.rf_a if stream_id == "A" else self.rf_b
        feats = []
        for det in detections:
            x1, y1, x2, y2 = det["x1"], det["y1"], det["x2"], det["y2"]
            if stream_id == "A":
                feats.append(self._extract_features_a(x1, y1, x2, y2))
            else:
                feats.append(self._extract_features_b(x1, y1, x2, y2))
        if feats:
            X = np.array(feats, dtype=np.float64)
            preds = model.predict(X)
            for i, pred in enumerate(preds):
                lon, lat = float(pred[0]), float(pred[1])
                lon, lat, valid = self._validate_output(lon, lat)
                if not valid:
                    self._error_count += 1
                results.append((lon, lat, valid))
        self._predict_count += len(results)
        return results

    def health(self):
        return {
            "loaded": self._loaded,
            "mode": "sklearn",
            "predict_count": self._predict_count,
            "error_count": self._error_count,
            "load_time_ms": self._load_time_ms,
        }

    def model_info(self):
        if not self._loaded:
            return {"loaded": False, "mode": "sklearn"}
        info_a = {
            "path": self.model_a_path,
            "n_trees": len(self.rf_a.estimators_),
            "n_features": self.rf_a.n_features_in_,
            "n_outputs": self.rf_a.n_outputs_,
        }
        info_b = {
            "path": self.model_b_path,
            "n_trees": len(self.rf_b.estimators_),
            "n_features": self.rf_b.n_features_in_,
            "n_outputs": self.rf_b.n_outputs_,
        }
        if hasattr(self.rf_a, "feature_names_in_"):
            info_a["feature_names"] = list(self.rf_a.feature_names_in_)
        if hasattr(self.rf_b, "feature_names_in_"):
            info_b["feature_names"] = list(self.rf_b.feature_names_in_)
        return {
            "loaded": True,
            "mode": "sklearn",
            "model_a": info_a,
            "model_b": info_b,
            "load_time_ms": self._load_time_ms,
        }

    def close(self):
        self.rf_a = None
        self.rf_b = None
        self._loaded = False
