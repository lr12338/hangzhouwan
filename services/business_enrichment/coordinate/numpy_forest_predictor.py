# -*- coding: utf-8 -*-
"""Numpy 向量化随机森林预测器。

无需 sklearn 运行时，通过 joblib 加载 pickle 后用纯 numpy 推理。
"""
import os
import sys
import types
import time

import numpy as np

from .base import CoordinatePredictor


class NumpyRandomForest:
    """纯 numpy 随机森林推理器。

    从 sklearn RandomForestRegressor pickle 中提取树结构，
    用向量化 numpy 遍历所有样本的所有树。
    """

    def __init__(self, model):
        n_trees = len(model.estimators_)
        self.n_trees = n_trees
        self.n_outputs = 0
        all_feat, all_thr, all_left, all_right, all_vals = [], [], [], [], []
        self.tree_offsets = [0]
        for est in model.estimators_:
            t = est.tree_
            n = t.node_count
            all_feat.append(t.nodes["feature"].astype(np.int32))
            all_thr.append(t.nodes["threshold"].astype(np.float64))
            all_left.append(t.nodes["left_child"].astype(np.int32))
            all_right.append(t.nodes["right_child"].astype(np.int32))
            all_vals.append(t.values.reshape(n, -1))
            self.tree_offsets.append(self.tree_offsets[-1] + n)
        self.feat = np.concatenate(all_feat)
        self.thr = np.concatenate(all_thr)
        self.left = np.concatenate(all_left)
        self.right = np.concatenate(all_right)
        self.vals = np.vstack(all_vals)
        self.n_outputs = self.vals.shape[1]
        self.root_nodes = np.array(self.tree_offsets[:-1], dtype=np.int32)
        for i in range(len(self.tree_offsets) - 1):
            s = slice(self.tree_offsets[i], self.tree_offsets[i + 1])
            off = self.tree_offsets[i]
            l = self.left[s]
            self.left[s] = np.where(l >= 0, l + off, -1)
            r = self.right[s]
            self.right[s] = np.where(r >= 0, r + off, -1)

    def predict(self, X):
        X = np.atleast_2d(np.asarray(X, dtype=np.float64))
        results = np.zeros((X.shape[0], self.n_outputs))
        for i in range(X.shape[0]):
            x = X[i]
            nodes = self.root_nodes.copy()
            while True:
                active = self.left[nodes] != -1
                if not active.any():
                    break
                f_idx = self.feat[nodes]
                t_val = self.thr[nodes]
                x_vals = x[f_idx]
                go_left = x_vals <= t_val
                nodes = np.where(
                    active,
                    np.where(go_left, self.left[nodes], self.right[nodes]),
                    nodes,
                )
            results[i] = self.vals[nodes].mean(axis=0)
        return results


def _load_without_sklearn(path):
    """用 stub 模块加载 sklearn pickle（无需 sklearn 安装）。"""
    import joblib

    class Stub:
        def __setstate__(self, s):
            if isinstance(s, dict):
                self.__dict__.update(s)
            elif isinstance(s, tuple):
                for i in s:
                    if isinstance(i, dict):
                        self.__dict__.update(i)

        def __new__(cls, *a, **k):
            return object.__new__(cls)

    for n in [
        "sklearn", "sklearn.ensemble", "sklearn.ensemble._forest",
        "sklearn.tree", "sklearn.tree._classes", "sklearn.tree._tree",
        "sklearn.utils", "sklearn.utils._joblib", "sklearn.base",
        "sklearn.exceptions", "sklearn._config",
        "scipy", "scipy.sparse",
    ]:
        if n not in sys.modules:
            m = types.ModuleType(n)
            m.__path__ = []
            sys.modules[n] = m
    sys.modules["sklearn.ensemble._forest"].RandomForestRegressor = type(
        "RFR", (Stub,), {}
    )
    sys.modules["sklearn.tree._classes"].DecisionTreeRegressor = type(
        "DTR", (Stub,), {}
    )
    sys.modules["sklearn.tree._tree"].Tree = type("Tree", (Stub,), {})
    sys.modules["scipy.sparse"].csr_matrix = type("csr", (Stub,), {})
    return joblib.load(path)


class NumpyForestCoordinatePredictor(CoordinatePredictor):
    """numpy 向量化坐标预测器。"""

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
        t0 = time.time()
        raw_a = _load_without_sklearn(self.model_a_path)
        raw_b = _load_without_sklearn(self.model_b_path)
        self.rf_a = NumpyRandomForest(raw_a)
        self.rf_b = NumpyRandomForest(raw_b)
        self._load_time_ms = round((time.time() - t0) * 1000, 2)
        self._loaded = True

    def predict(self, stream_id, detections):
        if not self._loaded:
            raise RuntimeError("predictor not loaded")
        results = []
        rf = self.rf_a if stream_id == "A" else self.rf_b
        feats = []
        for det in detections:
            x1, y1, x2, y2 = det["x1"], det["y1"], det["x2"], det["y2"]
            if stream_id == "A":
                feats.append(self._extract_features_a(x1, y1, x2, y2))
            else:
                feats.append(self._extract_features_b(x1, y1, x2, y2))
        if feats:
            X = np.array(feats, dtype=np.float64)
            preds = rf.predict(X)
            for pred in preds:
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
            "mode": "numpy",
            "predict_count": self._predict_count,
            "error_count": self._error_count,
            "load_time_ms": self._load_time_ms,
            "n_trees_a": self.rf_a.n_trees if self._loaded else 0,
            "n_trees_b": self.rf_b.n_trees if self._loaded else 0,
        }

    def model_info(self):
        if not self._loaded:
            return {"loaded": False, "mode": "numpy"}
        return {
            "loaded": True,
            "mode": "numpy",
            "model_a": {
                "path": self.model_a_path,
                "n_trees": self.rf_a.n_trees,
                "n_features": len(self.rf_a.feat) if self._loaded else 0,
                "n_outputs": self.rf_a.n_outputs,
            },
            "model_b": {
                "path": self.model_b_path,
                "n_trees": self.rf_b.n_trees,
                "n_features": len(self.rf_b.feat) if self._loaded else 0,
                "n_outputs": self.rf_b.n_outputs,
            },
            "load_time_ms": self._load_time_ms,
        }

    def close(self):
        self.rf_a = None
        self.rf_b = None
        self._loaded = False
