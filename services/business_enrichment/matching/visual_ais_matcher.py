# -*- coding: utf-8 -*-
"""视觉-AIS 匹配器：时间对齐 + SOG/COG 外推 + 全局距离排序。

拒绝原因：
  NO_AIS_DATA, AIS_STALE, COORDINATE_INVALID,
  DISTANCE_TOO_LARGE, ALREADY_MATCHED, SIDE_CAR_TIMEOUT
"""
import math
import time

EARTH_RADIUS_KM = 6371.0
EARTH_RADIUS_M = 6371000.0

REJECT_NO_AIS_DATA = "NO_AIS_DATA"
REJECT_AIS_STALE = "AIS_STALE"
REJECT_COORDINATE_INVALID = "COORDINATE_INVALID"
REJECT_DISTANCE_TOO_LARGE = "DISTANCE_TOO_LARGE"
REJECT_ALREADY_MATCHED = "ALREADY_MATCHED"
REJECT_SIDE_CAR_TIMEOUT = "SIDE_CAR_TIMEOUT"


def haversine_m(lon1, lat1, lon2, lat2):
    """haversine 距离（米）。"""
    lon1, lat1, lon2, lat2 = map(math.radians, [lon1, lat1, lon2, lat2])
    dlon = lon2 - lon1
    dlat = lat2 - lat1
    a = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
    return EARTH_RADIUS_M * c


def haversine_km(lon1, lat1, lon2, lat2):
    return haversine_m(lon1, lat1, lon2, lat2) / 1000.0


def extrapolate_position(lon, lat, speed_knots, course_deg, delta_sec):
    """使用 SOG/COG 外推位置。

    Args:
        lon, lat: 原始位置
        speed_knots: 速度（节）
        course_deg: 航向（度）
        delta_sec: 外推时间（秒），可为负

    Returns:
        (extrapolated_lon, extrapolated_lat)
    """
    if speed_knots is None or speed_knots <= 0 or delta_sec == 0:
        return lon, lat
    speed_ms = speed_knots * 0.514444  # 节 -> 米/秒
    distance_m = speed_ms * delta_sec
    if abs(distance_m) < 0.1:
        return lon, lat
    bearing = math.radians(course_deg if course_deg is not None else 0.0)
    lat_rad = math.radians(lat)
    lon_rad = math.radians(lon)
    angular_dist = distance_m / EARTH_RADIUS_M
    new_lat_rad = math.asin(
        math.sin(lat_rad) * math.cos(angular_dist) +
        math.cos(lat_rad) * math.sin(angular_dist) * math.cos(bearing)
    )
    new_lon_rad = lon_rad + math.atan2(
        math.sin(bearing) * math.sin(angular_dist) * math.cos(lat_rad),
        math.cos(angular_dist) - math.sin(lat_rad) * math.sin(new_lat_rad)
    )
    return math.degrees(new_lon_rad), math.degrees(new_lat_rad)


class VisualAisMatcher:
    """视觉-AIS 匹配器。

    匹配前：
      1. 计算 AIS 数据年龄
      2. 超过 data_timeout 拒绝
      3. 使用 SOG/COG 外推到视频帧时间
      4. 全局距离排序，一对一匹配
    """

    def __init__(self, stream_a_max_distance_m=500, stream_b_max_distance_m=500,
                 data_timeout_sec=30, max_extrapolation_sec=120):
        self.stream_max_distance = {
            "A": stream_a_max_distance_m,
            "B": stream_b_max_distance_m,
        }
        self.data_timeout_sec = data_timeout_sec
        self.max_extrapolation_sec = max_extrapolation_sec

    def match(self, stream_id, detections, ais_snapshot,
              frame_wall_time=None, coordinate_mode="sklearn"):
        """匹配一帧的检测结果。

        Args:
            stream_id: "A" or "B"
            detections: list of dict with detection_id, longitude, latitude, coordinate_valid
            ais_snapshot: dict mmsi -> AisRecord (or dict)
            frame_wall_time: 视频帧墙钟时间（epoch秒），None=now
            coordinate_mode: 当前坐标模式

        Returns:
            dict: detection_id -> match_result
        """
        now = frame_wall_time or time.time()
        max_dist_m = self.stream_max_distance.get(stream_id, 500)

        # 收集有效坐标的检测框
        valid_dets = []
        for det in detections:
            if not det.get("coordinate_valid", False):
                continue
            valid_dets.append(det)

        if not valid_dets:
            return {}

        if not ais_snapshot:
            return {}

        # 构建候选对（含外推）
        candidates = []
        for det in valid_dets:
            det_id = det["detection_id"]
            det_lon = det["longitude"]
            det_lat = det["latitude"]
            for mmsi, ais in ais_snapshot.items():
                if isinstance(ais, dict):
                    ais_dict = ais
                else:
                    ais_dict = ais.to_dict()
                ais_recv_ts = ais_dict.get("receive_timestamp", ais_dict.get("recv_ts", now))
                ais_age_sec = now - ais_recv_ts
                if ais_age_sec < 0:
                    # 未来时间，不使用
                    continue
                if ais_age_sec > self.data_timeout_sec:
                    continue
                ais_lon_raw = ais_dict.get("lon", 0)
                ais_lat_raw = ais_dict.get("lat", 0)
                speed = ais_dict.get("speed", 0)
                course = ais_dict.get("course", 0)
                # 外推到视频帧时间
                extrapolated = False
                ais_lon_aligned = ais_lon_raw
                ais_lat_aligned = ais_lat_raw
                if ais_age_sec > 1 and speed and speed > 0:
                    delta = ais_age_sec
                    if delta <= self.max_extrapolation_sec:
                        ais_lon_aligned, ais_lat_aligned = extrapolate_position(
                            ais_lon_raw, ais_lat_raw, speed, course, delta
                        )
                        extrapolated = True
                dist_m = haversine_m(det_lon, det_lat, ais_lon_aligned, ais_lat_aligned)
                if dist_m <= max_dist_m:
                    candidates.append((dist_m, det_id, mmsi, ais_dict,
                                       extrapolated, ais_age_sec,
                                       ais_lon_raw, ais_lat_raw,
                                       ais_lon_aligned, ais_lat_aligned))

        # 全局距离排序匹配
        candidates.sort(key=lambda x: x[0])
        matched_dets = set()
        matched_mmsi = set()
        results = {}
        for (dist_m, det_id, mmsi, ais_dict, extrapolated, ais_age_sec,
              ais_lon_raw, ais_lat_raw, ais_lon_aligned, ais_lat_aligned) in candidates:
            if det_id in matched_dets or mmsi in matched_mmsi:
                continue
            matched_dets.add(det_id)
            matched_mmsi.add(mmsi)
            # match_score: 距离越近分越高
            score = max(0.0, 1.0 - dist_m / max_dist_m)
            results[det_id] = {
                "matched": True,
                "mmsi": str(mmsi),
                "distance_m": round(dist_m, 2),
                "ais_age_ms": int(ais_age_sec * 1000),
                "coordinate_mode": coordinate_mode,
                "coordinate_valid": True,
                "extrapolated": extrapolated,
                "match_score": round(score, 4),
                "reject_reason": "",
                "speed": ais_dict.get("speed", 0),
                "course": ais_dict.get("course", 0),
                "ship_name": ais_dict.get("ship_name", ""),
                "ais_lon": round(ais_lon_raw, 6),
                "ais_lat": round(ais_lat_raw, 6),
                "ais_lon_aligned": round(ais_lon_aligned, 6),
                "ais_lat_aligned": round(ais_lat_aligned, 6),
            }
        return results


def match_detections_to_ais(detections, ais_snapshot, max_distance_km=0.5):
    """向后兼容旧接口。"""
    matcher = VisualAisMatcher(
        stream_a_max_distance_m=int(max_distance_km * 1000),
        stream_b_max_distance_m=int(max_distance_km * 1000),
    )
    # 转换 detections 为新格式
    new_dets = []
    for d in detections:
        new_dets.append({
            "detection_id": d["detection_id"],
            "longitude": d.get("longitude", 0),
            "latitude": d.get("latitude", 0),
            "coordinate_valid": d.get("coordinate_valid", False),
        })
    matched = matcher.match("A", new_dets, ais_snapshot)
    # 转换回旧格式
    results = {}
    for det_id, m in matched.items():
        results[det_id] = {
            "mmsi": m["mmsi"],
            "ship_name": m.get("ship_name", ""),
            "speed": m.get("speed", 0),
            "course": m.get("course", 0),
            "ais_distance_km": round(m["distance_m"] / 1000.0, 4),
            "ais_age_seconds": int(m["ais_age_ms"] / 1000),
            "ais_matched": True,
        }
    return results
