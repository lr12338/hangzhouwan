#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""审核坐标模型：生成 manifest.json，记录模型元数据。

用法：
  python3 tools/coordinate_model/audit_coordinate_models.py
"""
import hashlib
import json
import os
import subprocess
import sys
import time

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, _REPO_ROOT)

MODEL_A = os.environ.get("COORD_MODEL_A", "weights/0121_random_forest_model.pkl")
MODEL_B = os.environ.get("COORD_MODEL_B", "weights/beishang_x-l.pkl")
OUTPUT = os.environ.get("COORD_MANIFEST", "artifacts/coordinate-models/manifest.json")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def git_commit():
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            capture_output=True, text=True, cwd=_REPO_ROOT, timeout=5,
        )
        return result.stdout.strip()
    except Exception:
        return "unknown"


def audit_model(path, stream_id):
    import joblib
    t0 = time.time()
    with open(path, "rb") as f:
        data = f.read()
    sha = hashlib.sha256(data).hexdigest()
    model = joblib.load(path)
    load_ms = round((time.time() - t0) * 1000, 2)
    info = {
        "path": path,
        "sha256": sha,
        "size_bytes": len(data),
        "model_type": type(model).__name__,
        "n_trees": len(model.estimators_),
        "n_features": model.n_features_in_,
        "n_outputs": model.n_outputs_,
        "load_time_ms": load_ms,
    }
    if hasattr(model, "feature_names_in_"):
        info["feature_names"] = list(model.feature_names_in_)
    return info


def get_versions():
    import sklearn, scipy, joblib, numpy
    return {
        "python": "{}.{}.{}".format(*sys.version_info[:3]),
        "sklearn": sklearn.__version__,
        "scipy": scipy.__version__,
        "joblib": joblib.__version__,
        "numpy": numpy.__version__,
    }


def main():
    print(f"审核模型 A: {MODEL_A}")
    print(f"审核模型 B: {MODEL_B}")
    info_a = audit_model(MODEL_A, "A")
    info_b = audit_model(MODEL_B, "B")
    versions = get_versions()
    commit = git_commit()
    manifest = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "git_commit": commit,
        "versions": versions,
        "reference_resolution": {"width": 2560, "height": 1440},
        "model_a": info_a,
        "model_b": info_b,
    }
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    print(f"manifest 已写入: {OUTPUT}")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
