# -*- coding: utf-8 -*-
"""AIS 解码器：pyais 生产主路径 + 自研 6-bit 后备实现。

支持类型 1/2/3/18 位置消息，类型 4 基站消息。
"""
import abc
import re


def _decode_6bit(text):
    """6-bit ASCII -> bit array。"""
    bits = []
    for c in text:
        v = ord(c)
        if v < 88:
            val = v - 48
        else:
            val = v - 56
        val &= 0x3F
        bits.extend([(val >> (5 - i)) & 1 for i in range(6)])
    return bits


def _bits_to_uint(bits, start, length):
    val = 0
    for i in range(length):
        val = (val << 1) | bits[start + i]
    return val


def _bits_to_int(bits, start, length):
    val = _bits_to_uint(bits, start, length)
    if val & (1 << (length - 1)):
        val -= (1 << length)
    return val


class AisDecoder(abc.ABC):
    """AIS 解码器抽象接口。"""

    POSITION_TYPES = (1, 2, 3, 18)
    BASESTATION_TYPE = 4

    @abc.abstractmethod
    def decode(self, payload_str):
        """解码单条 AIS payload，返回 dict 或 None。

        Returns:
            dict with keys: msg_type, mmsi, lon, lat, speed, course,
                            heading, timestamp, decode_status, source_topic
            若不含位置信息返回 None
        """

    def decode_batch(self, messages):
        """批量解码。messages 为 list of str 或 list of (topic, payload) tuple。"""
        results = []
        for msg in messages:
            if isinstance(msg, tuple):
                topic, payload = msg
            else:
                topic, payload = "", msg
            result = self.decode(payload)
            if result is not None:
                result["source_topic"] = topic
            results.append(result)
        return results


class PyAisDecoder(AisDecoder):
    """使用 pyais 库解码（生产主路径）。"""

    def __init__(self):
        try:
            from pyais import decode
            self._decode = decode
            self._available = True
        except ImportError:
            self._available = False

    def decode(self, payload_str):
        if not self._available:
            return None
        stripped = payload_str.strip()
        # JSON 格式（如 dtu_shui_yu 主题）：直接解析预解码的 AIS 数据
        if stripped.startswith("{"):
            return self._decode_json(stripped)
        try:
            # pyais 2.4.0: decode() 返回单个消息对象（非可迭代）
            ts_match = re.search(r"\*(\d{10})$", stripped)
            timestamp = None
            if ts_match:
                timestamp = int(ts_match.group(1))
                stripped = stripped.replace(f"*{ts_match.group(1)}", "").strip()
            # 如果是完整 AIVDM 句子
            if "!" in stripped:
                msg = self._decode(stripped)
            else:
                # 构造一个最小 AIVDM 句子
                sentence = f"!AIVDM,1,1,,A,{stripped},0"
                msg = self._decode(sentence)
            if msg is None:
                return None
            mtype = msg.msg_type
            mmsi = str(msg.mmsi)
            result = {
                "msg_type": mtype,
                "mmsi": mmsi,
                "decode_status": "ok",
                "source_topic": "",
                "timestamp": timestamp,
            }
            if mtype in self.POSITION_TYPES:
                lon = msg.lon if hasattr(msg, "lon") else None
                lat = msg.lat if hasattr(msg, "lat") else None
                speed = msg.speed if hasattr(msg, "speed") else 0.0
                course = msg.course if hasattr(msg, "course") else 0.0
                heading = msg.heading if hasattr(msg, "heading") else 511
                if lon is None or lat is None:
                    return None
                result.update({
                    "lon": float(lon),
                    "lat": float(lat),
                    "speed": float(speed) if speed is not None else 0.0,
                    "course": float(course) if course is not None else 0.0,
                    "heading": int(heading) if heading is not None else 511,
                })
            elif mtype == self.BASESTATION_TYPE:
                lon = msg.lon if hasattr(msg, "lon") else None
                lat = msg.lat if hasattr(msg, "lat") else None
                if lon is not None and lat is not None:
                    result.update({
                        "lon": float(lon),
                        "lat": float(lat),
                        "speed": 0.0,
                        "course": 0.0,
                        "heading": 511,
                    })
                else:
                    return None
            elif mtype in (5, 24):
                # 静态数据报告（船名等），无位置信息
                shipname = getattr(msg, "shipname", None) or getattr(msg, "name", None)
                if shipname:
                    result["ship_name"] = str(shipname).strip()
                return result
            else:
                return None
            return self._validate(result)
        except Exception:
            return None

    @staticmethod
    def _decode_json(stripped):
        """解析 JSON 格式的预解码 AIS 数据（如 dtu_shui_yu 主题）。

        JSON 字段：mmsi, longitude, latitude, speed, course, heading, time
        """
        import json as _json
        try:
            d = _json.loads(stripped)
            lon = d.get("longitude")
            lat = d.get("latitude")
            if lon is None or lat is None:
                return None
            lon = float(lon)
            lat = float(lat)
            if lon == 0 and lat == 0:
                return None
            if not (-180 <= lon <= 180) or not (-90 <= lat <= 90):
                return None
            res = {
                "msg_type": 1,
                "mmsi": str(d.get("mmsi", "")),
                "lon": lon,
                "lat": lat,
                "speed": float(d.get("speed", 0.0) or 0.0),
                "course": float(d.get("course", 0.0) or 0.0),
                "heading": int(d.get("heading", 511) or 511),
                "decode_status": "ok",
                "source_topic": "",
                "timestamp": None,
            }
            ship_name = d.get("shipName") or d.get("shipname") or d.get("name")
            if ship_name:
                res["ship_name"] = str(ship_name).strip()
            return res
        except Exception:
            return None

    @staticmethod
    def _validate(result):
        if "lon" not in result or "lat" not in result:
            return None
        lon, lat = result["lon"], result["lat"]
        if not (-180 <= lon <= 180) or not (-90 <= lat <= 90):
            return None
        if lon == 0 and lat == 0:
            return None
        return result


class LightweightAisDecoder(AisDecoder):
    """自研 6-bit 解码器（后备实现）。

    无需 pyais 依赖，支持类型 1/2/3/4/18。
    """

    def decode(self, payload_str):
        try:
            stripped = payload_str.strip()
            ts_match = re.search(r"\*(\d{10})$", stripped)
            timestamp = None
            if ts_match:
                timestamp = int(ts_match.group(1))
                stripped = stripped.replace(f"*{ts_match.group(1)}", "").strip()
            # 如果是完整 NMEA 句子，提取 payload 字段
            if stripped.startswith("!"):
                parts = stripped.split(",")
                if len(parts) >= 6:
                    stripped = parts[5]
            bits = _decode_6bit(stripped)
            if len(bits) < 38:
                return None
            msg_type = _bits_to_uint(bits, 0, 6)
            mmsi = _bits_to_uint(bits, 8, 30)
            result = {
                "msg_type": msg_type,
                "mmsi": str(mmsi),
                "decode_status": "ok",
                "source_topic": "",
                "timestamp": timestamp,
            }
            if msg_type in (1, 2, 3):
                if len(bits) < 128:
                    return None
                speed = _bits_to_uint(bits, 50, 10) / 10.0
                lon = _bits_to_int(bits, 61, 28) / 600000.0
                lat = _bits_to_int(bits, 89, 27) / 600000.0
                course = _bits_to_uint(bits, 116, 12) / 10.0
                heading = _bits_to_uint(bits, 128, 9) if len(bits) >= 137 else 511
                result.update({"lon": lon, "lat": lat, "speed": speed,
                               "course": course, "heading": heading})
            elif msg_type == 18:
                if len(bits) < 128:
                    return None
                speed = _bits_to_uint(bits, 50, 10) / 10.0
                lon = _bits_to_int(bits, 61, 28) / 600000.0
                lat = _bits_to_int(bits, 89, 27) / 600000.0
                course = _bits_to_uint(bits, 116, 12) / 10.0
                heading = _bits_to_uint(bits, 128, 9) if len(bits) >= 137 else 511
                result.update({"lon": lon, "lat": lat, "speed": speed,
                               "course": course, "heading": heading})
            elif msg_type == 4:
                if len(bits) < 134:
                    return None
                lon = _bits_to_int(bits, 79, 28) / 600000.0
                lat = _bits_to_int(bits, 107, 27) / 600000.0
                result.update({"lon": lon, "lat": lat, "speed": 0.0,
                               "course": 0.0, "heading": 511})
            else:
                return None
            return self._validate(result)
        except Exception:
            return None

    @staticmethod
    def _validate(result):
        if "lon" not in result or "lat" not in result:
            return None
        lon, lat = result["lon"], result["lat"]
        if not (-180 <= lon <= 180) or not (-90 <= lat <= 90):
            return None
        if lon == 0 and lat == 0:
            return None
        return result


# 向后兼容旧接口
_default_decoder = None


def _get_decoder():
    global _default_decoder
    if _default_decoder is None:
        _default_decoder = PyAisDecoder()
        if not _default_decoder._available:
            _default_decoder = LightweightAisDecoder()
    return _default_decoder


def parse_ais_payload(payload_str):
    """向后兼容旧接口：返回 dict 或 None。"""
    result = _get_decoder().decode(payload_str)
    if result is None:
        return None
    # 保持旧字段名兼容
    out = {"msg_type": result["msg_type"], "mmsi": result["mmsi"]}
    if "lon" in result:
        out["lon"] = result["lon"]
        out["lat"] = result["lat"]
        out["speed"] = result.get("speed", 0.0)
        out["course"] = result.get("course", 0.0)
    return out
