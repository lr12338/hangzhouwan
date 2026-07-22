# -*- coding: utf-8 -*-
"""AIS 证据完整性单元测试。

验证 VisualAisMatcher 返回真实 AIS 坐标（不写 0），证据字段完整。
"""
import math
import os
import sys
import time
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from services.business_enrichment.matching.visual_ais_matcher import (
    VisualAisMatcher, haversine_m, extrapolate_position,
)


class TestAisEvidence(unittest.TestCase):
    """验证 AIS 证据字段完整且坐标真实。"""

    def _make_ais(self, mmsi, lon, lat, speed=0, course=0, recv_ts=None):
        return {
            "mmsi": mmsi,
            "lon": lon,
            "lat": lat,
            "speed": speed,
            "course": course,
            "receive_timestamp": recv_ts if recv_ts is not None else time.time(),
            "ship_name": "测试船{}".format(mmsi),
        }

    def test_match_returns_real_ais_coordinates(self):
        """匹配结果包含真实 AIS 坐标（非 0）。"""
        now = time.time()
        matcher = VisualAisMatcher(stream_a_max_distance_m=500)
        detections = [{
            "detection_id": 0,
            "longitude": 121.0548,
            "latitude": 30.5688,
            "coordinate_valid": True,
        }]
        ais_snap = {
            "412000001": self._make_ais("412000001", 121.0540, 30.5690, recv_ts=now),
        }
        results = matcher.match("A", detections, ais_snap, frame_wall_time=now)
        self.assertIn(0, results)
        r = results[0]
        self.assertTrue(r["matched"])
        # AIS 原始坐标非 0
        self.assertNotEqual(r["ais_lon"], 0)
        self.assertNotEqual(r["ais_lat"], 0)
        self.assertAlmostEqual(r["ais_lon"], 121.0540, places=3)
        self.assertAlmostEqual(r["ais_lat"], 30.5690, places=3)
        # 时间对齐后坐标
        self.assertNotEqual(r["ais_lon_aligned"], 0)
        self.assertNotEqual(r["ais_lat_aligned"], 0)

    def test_extrapolation_produces_aligned_coords(self):
        """外推时对齐坐标与原始坐标不同。"""
        now = time.time()
        matcher = VisualAisMatcher(stream_a_max_distance_m=5000,
                                   data_timeout_sec=60)
        detections = [{
            "detection_id": 0,
            "longitude": 121.060,
            "latitude": 30.570,
            "coordinate_valid": True,
        }]
        # AIS 数据 10 秒前，船速 20 节
        ais_snap = {
            "412000002": self._make_ais("412000002", 121.055, 30.569,
                                        speed=20, course=90,
                                        recv_ts=now - 10),
        }
        results = matcher.match("A", detections, ais_snap, frame_wall_time=now)
        self.assertIn(0, results)
        r = results[0]
        self.assertTrue(r["extrapolated"])
        # 原始坐标保留
        self.assertAlmostEqual(r["ais_lon"], 121.055, places=3)
        # 对齐后坐标应不同（外推了）
        self.assertNotAlmostEqual(r["ais_lon_aligned"], r["ais_lon"], places=4)

    def test_evidence_fields_complete(self):
        """证据字段完整：MMSI、船名、距离、年龄、match_score、reject_reason。"""
        now = time.time()
        matcher = VisualAisMatcher(stream_a_max_distance_m=500)
        detections = [{
            "detection_id": 0,
            "longitude": 121.0548,
            "latitude": 30.5688,
            "coordinate_valid": True,
        }]
        ais_snap = {
            "412000003": self._make_ais("412000003", 121.054, 30.569, recv_ts=now),
        }
        results = matcher.match("A", detections, ais_snap, frame_wall_time=now)
        r = results[0]
        for field in ("mmsi", "ship_name", "distance_m", "ais_age_ms",
                      "match_score", "reject_reason", "ais_lon", "ais_lat",
                      "ais_lon_aligned", "ais_lat_aligned", "extrapolated"):
            self.assertIn(field, r, "missing field: %s" % field)
        self.assertEqual(r["mmsi"], "412000003")
        self.assertEqual(r["ship_name"], "测试船412000003")
        self.assertGreater(r["distance_m"], 0)
        self.assertGreaterEqual(r["match_score"], 0)
        self.assertEqual(r["reject_reason"], "")

    def test_no_ais_data_returns_empty(self):
        """无 AIS 数据时不返回匹配（不伪造坐标）。"""
        matcher = VisualAisMatcher()
        detections = [{
            "detection_id": 0,
            "longitude": 121.0,
            "latitude": 30.5,
            "coordinate_valid": True,
        }]
        results = matcher.match("A", detections, {})
        self.assertEqual(results, {})

    def test_one_to_one_matching(self):
        """一对一匹配：两个检测框匹配两个不同 MMSI。"""
        now = time.time()
        matcher = VisualAisMatcher(stream_a_max_distance_m=1000)
        detections = [
            {"detection_id": 0, "longitude": 121.054, "latitude": 30.569,
             "coordinate_valid": True},
            {"detection_id": 1, "longitude": 121.060, "latitude": 30.575,
             "coordinate_valid": True},
        ]
        ais_snap = {
            "412000010": self._make_ais("412000010", 121.054, 30.569, recv_ts=now),
            "412000011": self._make_ais("412000011", 121.060, 30.575, recv_ts=now),
        }
        results = matcher.match("A", detections, ais_snap, frame_wall_time=now)
        self.assertEqual(len(results), 2)
        mmsis = {r["mmsi"] for r in results.values()}
        self.assertEqual(mmsis, {"412000010", "412000011"})


if __name__ == "__main__":
    unittest.main()
