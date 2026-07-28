# -*- coding: utf-8 -*-
"""JSONL 业务证据格式单元测试。

验证 C++ 管线输出的 JSONL 每行可被标准 JSON 解析，且结构合法。
"""
import json
import os
import sys
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)


def make_jsonl_line(stream_id, frame_sequence, timestamp_ms, coordinate_mode,
                    enrichment_status, detections):
    """模拟 C++ 管线输出的 JSONL 行格式。"""
    dets = []
    for e in detections:
        d = {
            "detection_id": e["detection_id"],
            "x1": e["x1"], "y1": e["y1"], "x2": e["x2"], "y2": e["y2"],
            "score": e["score"],
            "longitude": e["longitude"], "latitude": e["latitude"],
            "coordinate_valid": e["coordinate_valid"],
            "ais_matched": e["ais_matched"],
            "speed": e["speed"], "course": e["course"],
            "ais_distance_m": e["ais_distance_m"],
            "ais_age_ms": e["ais_age_ms"],
            "match_score": e["match_score"],
            "extrapolated": e["extrapolated"],
            "enrichment_status": e["enrichment_status"],
            "mmsi": e["mmsi"],
            "ship_name": e["ship_name"],
            "reject_reason": e["reject_reason"],
        }
        if e["ais_matched"]:
            d["ais_lon"] = e["ais_lon"]
            d["ais_lat"] = e["ais_lat"]
            d["ais_lon_aligned"] = e["ais_lon_aligned"]
            d["ais_lat_aligned"] = e["ais_lat_aligned"]
        dets.append(d)
    return json.dumps({
        "schema_version": 1,
        "stream_id": stream_id,
        "frame_sequence": frame_sequence,
        "timestamp_ms": timestamp_ms,
        "coordinate_mode": coordinate_mode,
        "enrichment_status": enrichment_status,
        "detections": dets,
    }, ensure_ascii=False)


class TestJsonlFormat(unittest.TestCase):
    """验证 JSONL 每行可被标准 JSON 解析且结构合法。"""

    def _base_det(self, **overrides):
        d = {
            "detection_id": 0, "x1": 100, "y1": 200, "x2": 300, "y2": 400,
            "score": 0.95, "longitude": 121.05, "latitude": 30.57,
            "coordinate_valid": True, "ais_matched": True,
            "speed": 5.0, "course": 180.0, "ais_distance_m": 84.0,
            "ais_age_ms": 1200, "match_score": 0.83, "extrapolated": False,
            "enrichment_status": "FULL", "mmsi": "412000001",
            "ship_name": "测试船\"名", "reject_reason": "",
            "ais_lon": 121.054, "ais_lat": 30.569,
            "ais_lon_aligned": 121.0541, "ais_lat_aligned": 30.5691,
        }
        d.update(overrides)
        return d

    def test_single_line_parses(self):
        """单行 JSONL 可被 json.loads 解析。"""
        line = make_jsonl_line("A", 123, 1000, "sklearn", "FULL",
                               [self._base_det()])
        obj = json.loads(line)
        self.assertEqual(obj["schema_version"], 1)
        self.assertEqual(obj["stream_id"], "A")
        self.assertEqual(obj["frame_sequence"], 123)
        self.assertEqual(obj["timestamp_ms"], 1000)
        self.assertEqual(obj["coordinate_mode"], "sklearn")
        self.assertEqual(obj["enrichment_status"], "FULL")
        self.assertIsInstance(obj["detections"], list)
        self.assertEqual(len(obj["detections"]), 1)

    def test_detections_structure(self):
        """detections 数组结构完整。"""
        dets = [
            self._base_det(detection_id=0, ais_matched=True),
            self._base_det(detection_id=1, ais_matched=False,
                           enrichment_status="COORD_ONLY",
                           mmsi="", ship_name="",
                           coordinate_valid=True),
            self._base_det(detection_id=2, ais_matched=False,
                           enrichment_status="DETECTION_ONLY",
                           mmsi="", ship_name="",
                           coordinate_valid=False,
                           coordinate_mode_override="off"),
        ]
        line = make_jsonl_line("A", 1, 100, "sklearn", "FULL", dets)
        obj = json.loads(line)
        self.assertEqual(len(obj["detections"]), 3)
        self.assertTrue(obj["detections"][0]["ais_matched"])
        self.assertFalse(obj["detections"][1]["ais_matched"])
        self.assertFalse(obj["detections"][2]["ais_matched"])
        # AIS 匹配的才有 ais_lon/ais_lat
        self.assertIn("ais_lon", obj["detections"][0])
        self.assertNotIn("ais_lon", obj["detections"][1])

    def test_ship_name_escaping(self):
        """船名含特殊字符时正确转义，可被 JSON 解析。"""
        det = self._base_det(ship_name='船名"含\\换行\n引号')
        line = make_jsonl_line("B", 5, 200, "sklearn", "FULL", [det])
        obj = json.loads(line)
        self.assertEqual(obj["detections"][0]["ship_name"], '船名"含\\换行\n引号')

    def test_unicode_ship_name(self):
        """中文船名正确处理。"""
        det = self._base_det(ship_name="浙杭州货001")
        line = make_jsonl_line("A", 10, 300, "sklearn", "FULL", [det])
        obj = json.loads(line)
        self.assertEqual(obj["detections"][0]["ship_name"], "浙杭州货001")

    def test_empty_detections(self):
        """空 detections 数组合法。"""
        line = make_jsonl_line("A", 0, 0, "sklearn", "DETECTION_ONLY", [])
        obj = json.loads(line)
        self.assertEqual(obj["detections"], [])

    def test_multiline_jsonl(self):
        """多行 JSONL 每行独立可解析。"""
        lines = []
        for i in range(5):
            lines.append(make_jsonl_line("A", i, i * 100, "sklearn", "FULL",
                                         [self._base_det(detection_id=i)]))
        for line in lines:
            obj = json.loads(line)
            self.assertEqual(obj["frame_sequence"], lines.index(line))

    def test_includes_coordinate_mode(self):
        """必须包含 coordinate_mode 字段。"""
        line = make_jsonl_line("A", 1, 100, "numpy", "COORD_ONLY",
                               [self._base_det(enrichment_status="COORD_ONLY",
                                               ais_matched=False)])
        obj = json.loads(line)
        self.assertEqual(obj["coordinate_mode"], "numpy")

    def test_includes_enrichment_status(self):
        """必须包含 enrichment_status 字段。"""
        line = make_jsonl_line("A", 1, 100, "sklearn", "DETECTION_ONLY",
                               [self._base_det(enrichment_status="DETECTION_ONLY",
                                               ais_matched=False)])
        obj = json.loads(line)
        self.assertEqual(obj["enrichment_status"], "DETECTION_ONLY")

    def test_includes_reject_reason(self):
        """必须包含 reject_reason 字段。"""
        det = self._base_det(reject_reason="DISTANCE_TOO_LARGE", ais_matched=False,
                             enrichment_status="COORD_ONLY")
        line = make_jsonl_line("A", 1, 100, "sklearn", "COORD_ONLY", [det])
        obj = json.loads(line)
        self.assertEqual(obj["detections"][0]["reject_reason"], "DISTANCE_TOO_LARGE")


if __name__ == "__main__":
    unittest.main()
