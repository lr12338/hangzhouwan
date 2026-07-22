# -*- coding: utf-8 -*-
"""RPC 协议定义（版本2）。

长度前缀 + JSONL framing，支持请求ID、超时、协议版本协商。
"""
import json
import struct
import os

PROTOCOL_VERSION = "2"
MAX_REQUEST_SIZE = 4 * 1024 * 1024  # 4MB
MAX_DETECTIONS = 256

HEADER_FMT = "!I"  # 4-byte unsigned int (network byte order)
HEADER_SIZE = 4


class ProtocolError(Exception):
    pass


def encode_message(obj):
    """将 dict 编码为长度前缀 + JSONL 帧。"""
    payload = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    if len(payload) > MAX_REQUEST_SIZE:
        raise ProtocolError(f"message too large: {len(payload)} > {MAX_REQUEST_SIZE}")
    return struct.pack(HEADER_FMT, len(payload)) + payload


def decode_stream(buf):
    """从缓冲区解码完整消息，返回 (obj, remaining_bytes) 或 (None, buf) 表示数据不足。"""
    if len(buf) < HEADER_SIZE:
        return None, buf
    (msg_len,) = struct.unpack(HEADER_FMT, buf[:HEADER_SIZE])
    if msg_len > MAX_REQUEST_SIZE:
        raise ProtocolError(f"declared message too large: {msg_len}")
    total = HEADER_SIZE + msg_len
    if len(buf) < total:
        return None, buf
    payload = buf[HEADER_SIZE:total]
    remaining = buf[total:]
    try:
        obj = json.loads(payload.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        raise ProtocolError(f"invalid json: {e}")
    return obj, remaining


def make_request(stream_id, frame_sequence, image_width, image_height,
                 detections, source_epoch=None, deadline_ms=30,
                 coordinate_mode=None, request_id=None):
    """构造标准请求。"""
    if request_id is None:
        request_id = f"{os.getpid()}-{frame_sequence}-{int(__import__('time').time()*1000)}"
    if len(detections) > MAX_DETECTIONS:
        detections = detections[:MAX_DETECTIONS]
    return {
        "protocol_version": PROTOCOL_VERSION,
        "request_id": request_id,
        "stream_id": stream_id,
        "source_epoch": source_epoch or 0,
        "frame_sequence": frame_sequence,
        "deadline_ms": deadline_ms,
        "coordinate_mode": coordinate_mode or "",
        "image_width": image_width,
        "image_height": image_height,
        "detections": detections,
    }


def make_response(stream_id, frame_sequence, results, processing_ms,
                  coordinate_mode="", response_status="OK", error_code="",
                  request_id=""):
    """构造标准响应。"""
    return {
        "protocol_version": PROTOCOL_VERSION,
        "request_id": request_id,
        "stream_id": stream_id,
        "frame_sequence": frame_sequence,
        "coordinate_mode": coordinate_mode,
        "response_status": response_status,
        "error_code": error_code,
        "processing_ms": processing_ms,
        "results": results,
    }


# 向后兼容：旧版换行分隔 JSONL（协议版本1）的辅助函数
def encode_line_json(obj):
    """旧版换行分隔 JSONL（协议1）。"""
    return (json.dumps(obj, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")


def decode_line_json(buf):
    """旧版换行分隔解码。返回 (obj, remaining) 或 (None, buf)。"""
    idx = buf.find(b"\n")
    if idx < 0:
        return None, buf
    line = buf[:idx].strip()
    remaining = buf[idx + 1:]
    if not line:
        return None, remaining
    try:
        obj = json.loads(line.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return "INVALID", remaining
    return obj, remaining
