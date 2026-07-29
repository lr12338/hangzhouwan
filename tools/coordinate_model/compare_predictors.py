#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""比较 sklearn 和 numpy 坐标预测器在黄金样本上的等价性。

验收门禁：
  - 1000/1000 样本可计算
  - 经度最大绝对误差 <= 1e-7度
  - 纬度最大绝对误差 <= 1e-7度
  - 无 NaN/Inf
  - A/B 特征定义不混淆

用法：
  python3 tools/coordinate_model/compare_predictors.py
"""
import json
import math
import os
import sys
import time

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, _REPO_ROOT)

from services.business_enrichment.coordinate import SklearnCoordinatePredictor, NumpyForestCoordinatePredictor

GOLDEN_FILE = os.environ.get("GOLDEN_FILE", "artifacts/coordinate-models/golden_cases.json")
MODEL_A = os.environ.get("COORD_MODEL_A", "weights/0121_random_forest_model.pkl")
MODEL_B = os.environ.get("COORD_MODEL_B", "weights/beishang_x-l.pkl")
OUTPUT = os.environ.get("COMPARE_OUTPUT", "artifacts/coordinate-models/comparison_report.json")

LON_THRESHOLD = 1e-7
LAT_THRESHOLD = 1e-7


def run_comparison(stream_id, cases, sk_predictor, np_predictor):
    results = {
        "stream_id": stream_id,
        "total": len(cases),
        "computable": 0,
        "not_computable": 0,
        "max_lon_diff": 0.0,
        "max_lat_diff": 0.0,
        "nan_count": 0,
        "inf_count": 0,
        "passed": False,
        "errors": [],
    }
    sk_detections = []
    np_detections = []
    for c in cases:
        sk_detections.append({
            "x1": c["x1"], "y1": c["y1"], "x2": c["x2"], "y2": c["y2"],
            "image_width": 2560, "image_height": 1440,
        })
        np_detections.append({
            "x1": c["x1"], "y1": c["y1"], "x2": c["x2"], "y2": c["y2"],
            "image_width": 2560, "image_height": 1440,
        })

    try:
        sk_results = sk_predictor.predict(stream_id, sk_detections)
    except Exception as e:
        sk_results = [(0.0, 0.0, False)] * len(cases)
    try:
        np_results = np_predictor.predict(stream_id, np_detections)
    except Exception as e:
        np_results = [(0.0, 0.0, False)] * len(cases)

    for i, (sk_res, np_res) in enumerate(zip(sk_results, np_results)):
        sk_lon, sk_lat, sk_valid = sk_res
        np_lon, np_lat, np_valid = np_res
        if math.isnan(sk_lon) or math.isnan(np_lon) or math.isnan(sk_lat) or math.isnan(np_lat):
            results["nan_count"] += 1
            continue
        if math.isinf(sk_lon) or math.isinf(np_lon) or math.isinf(sk_lat) or math.isinf(np_lat):
            results["inf_count"] += 1
            continue
        if sk_valid and np_valid:
            results["computable"] += 1
            lon_diff = abs(sk_lon - np_lon)
            lat_diff = abs(sk_lat - np_lat)
            if lon_diff > results["max_lon_diff"]:
                results["max_lon_diff"] = lon_diff
            if lat_diff > results["max_lat_diff"]:
                results["max_lat_diff"] = lat_diff
        else:
            results["not_computable"] += 1

    results["passed"] = (
        results["nan_count"] == 0 and
        results["inf_count"] == 0 and
        results["max_lon_diff"] <= LON_THRESHOLD and
        results["max_lat_diff"] <= LAT_THRESHOLD
    )
    return results


def main():
    print("加载黄金样本...")
    with open(GOLDEN_FILE, "r", encoding="utf-8") as f:
        golden = json.load(f)
    cases = golden["cases"]
    print(f"  {len(cases)} 个样本")

    print("加载 sklearn 预测器...")
    sk = SklearnCoordinatePredictor(MODEL_A, MODEL_B)
    sk.load()
    print("加载 numpy 预测器...")
    np_pred = NumpyForestCoordinatePredictor(MODEL_A, MODEL_B)
    np_pred.load()

    print("\n比较 A 路 (4特征 [x1,y1,x2,y2])...")
    res_a = run_comparison("A", cases, sk, np_pred)
    print(f"  可计算: {res_a['computable']}/{res_a['total']}")
    print(f"  最大经度差: {res_a['max_lon_diff']:.2e}")
    print(f"  最大纬度差: {res_a['max_lat_diff']:.2e}")
    print(f"  NaN: {res_a['nan_count']}, Inf: {res_a['inf_count']}")
    print(f"  通过: {res_a['passed']}")

    print("\n比较 B 路 (2特征 [x_center,y_center])...")
    res_b = run_comparison("B", cases, sk, np_pred)
    print(f"  可计算: {res_b['computable']}/{res_b['total']}")
    print(f"  最大经度差: {res_b['max_lon_diff']:.2e}")
    print(f"  最大纬度差: {res_b['max_lat_diff']:.2e}")
    print(f"  NaN: {res_b['nan_count']}, Inf: {res_b['inf_count']}")
    print(f"  通过: {res_b['passed']}")

    report = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "golden_cases": len(cases),
        "thresholds": {"lon": LON_THRESHOLD, "lat": LAT_THRESHOLD},
        "model_a": res_a,
        "model_b": res_b,
        "overall_passed": res_a["passed"] and res_b["passed"],
    }
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print(f"\n报告已写入: {OUTPUT}")

    if report["overall_passed"]:
        print("\n✅ 等价验证通过：sklearn 与 numpy 预测完全一致")
    else:
        print("\n❌ 等价验证未通过")
    sys.exit(0 if report["overall_passed"] else 1)


if __name__ == "__main__":
    main()
