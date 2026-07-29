# -*- coding: utf-8 -*-
"""线程安全、容量受控的 AIS 缓存。

每条记录保存：ais_timestamp, receive_timestamp, longitude, latitude,
speed, course, heading, source_topic, decode_status。
"""
import threading
import time


class AisRecord:
    __slots__ = (
        "mmsi", "lon", "lat", "speed", "course", "heading",
        "ais_timestamp", "receive_timestamp", "source_topic", "decode_status",
        "ship_name",
    )

    def __init__(self, mmsi, lon, lat, speed, course, heading,
                 ais_timestamp, receive_timestamp, source_topic="", decode_status="ok",
                 ship_name=""):
        self.mmsi = str(mmsi)
        self.lon = lon
        self.lat = lat
        self.speed = speed
        self.course = course
        self.heading = heading
        self.ais_timestamp = ais_timestamp or receive_timestamp
        self.receive_timestamp = receive_timestamp
        self.source_topic = source_topic
        self.decode_status = decode_status
        self.ship_name = ship_name or ""

    def to_dict(self):
        return {
            "mmsi": self.mmsi,
            "lon": self.lon,
            "lat": self.lat,
            "speed": self.speed,
            "course": self.course,
            "heading": self.heading,
            "ais_timestamp": self.ais_timestamp,
            "receive_timestamp": self.receive_timestamp,
            "source_topic": self.source_topic,
            "decode_status": self.decode_status,
            "ship_name": self.ship_name,
        }


class AisStore:
    """线程安全 AIS 缓存，支持过期清理和容量限制。"""

    def __init__(self, max_capacity=500, timeout_sec=30):
        self._lock = threading.Lock()
        self._data = {}
        self._max = max_capacity
        self._timeout = timeout_sec
        self.msg_count = 0
        self.parse_ok = 0
        self.parse_fail = 0
        self.msg_type_counts = {}
        self._names = {}  # mmsi -> ship_name（静态数据报告缓存）

    def update(self, mmsi, lon, lat, speed, course, heading=511,
               ais_timestamp=None, source_topic="", decode_status="ok",
               ship_name=""):
        with self._lock:
            recv_ts = time.time()
            mmsi = str(mmsi)
            if len(self._data) >= self._max and mmsi not in self._data:
                oldest = min(self._data, key=lambda k: self._data[k].receive_timestamp)
                del self._data[oldest]
            if not ship_name:
                ship_name = self._names.get(mmsi, "")
            self._data[mmsi] = AisRecord(
                mmsi, lon, lat, speed, course, heading,
                ais_timestamp, recv_ts, source_topic, decode_status, ship_name
            )

    def update_ship_name(self, mmsi, ship_name):
        """更新/缓存船名（来自 type-5/24 静态数据报告或 JSON）。"""
        with self._lock:
            mmsi = str(mmsi)
            self._names[mmsi] = ship_name
            rec = self._data.get(mmsi)
            if rec is not None and not rec.ship_name:
                rec.ship_name = ship_name

    def update_from_decoded(self, decoded):
        """从解码器输出更新缓存。"""
        with self._lock:
            self.msg_count += 1
        if decoded is None:
            with self._lock:
                self.parse_fail += 1
            return
        mtype = decoded.get("msg_type", 0)
        with self._lock:
            self.msg_type_counts[mtype] = self.msg_type_counts.get(mtype, 0) + 1
        mmsi = str(decoded.get("mmsi", ""))
        ship_name = decoded.get("ship_name", "")
        if ship_name:
            self.update_ship_name(mmsi, ship_name)
        if "lon" not in decoded:
            # 仅静态数据（如 type-5/24 船名），无位置：视为成功解析
            with self._lock:
                self.parse_ok += 1
            return
        self.update(
            decoded["mmsi"],
            decoded["lon"], decoded["lat"],
            decoded.get("speed", 0.0),
            decoded.get("course", 0.0),
            decoded.get("heading", 511),
            decoded.get("timestamp"),
            decoded.get("source_topic", ""),
            decoded.get("decode_status", "ok"),
            ship_name or "",
        )
        with self._lock:
            self.parse_ok += 1

    def snapshot(self):
        """返回未过期记录的 dict (mmsi -> AisRecord)。"""
        with self._lock:
            now = time.time()
            return {
                k: v for k, v in self._data.items()
                if now - v.receive_timestamp < self._timeout
            }

    def snapshot_with_age(self, frame_wall_time=None):
        """返回记录列表，每条附带 ais_age_ms。"""
        with self._lock:
            now = time.time()
            ref = frame_wall_time or now
            result = []
            for k, v in self._data.items():
                age = ref - v.receive_timestamp
                if age < self._timeout:
                    d = v.to_dict()
                    d["ais_age_ms"] = int(age * 1000)
                    result.append(d)
            return result

    def cleanup(self):
        with self._lock:
            now = time.time()
            expired = [k for k, v in self._data.items()
                       if now - v.receive_timestamp >= self._timeout]
            for k in expired:
                del self._data[k]
            return len(expired)

    def count(self):
        with self._lock:
            return len(self._data)

    def stats(self):
        with self._lock:
            return {
                "msg_count": self.msg_count,
                "parse_ok": self.parse_ok,
                "parse_fail": self.parse_fail,
                "cache_count": len(self._data),
                "msg_type_counts": dict(self.msg_type_counts),
            }
