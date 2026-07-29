# -*- coding: utf-8 -*-
"""MQTT AIS 原始消息录制工具。

录制格式 JSONL:
{
  "topic": "",
  "receive_time_ms": 0,
  "payload": ""
}

按时间或大小轮转，禁止无限增长。
"""
import json
import os
import time


class AisCapture:
    """AIS 原始消息录制器，支持按大小和时间轮转。"""

    def __init__(self, output_dir, max_size_mb=50, max_files=10,
                 rotate_interval_sec=3600):
        self.output_dir = output_dir
        self.max_size_bytes = max_size_mb * 1024 * 1024
        self.max_files = max_files
        self.rotate_interval_sec = rotate_interval_sec
        self._file = None
        self._file_path = None
        self._file_size = 0
        self._file_start_time = 0
        self._count = 0

    def _open_new(self):
        os.makedirs(self.output_dir, exist_ok=True)
        ts = int(time.time())
        self._file_path = os.path.join(
            self.output_dir, f"ais_capture_{ts}.jsonl"
        )
        self._file = open(self._file_path, "w", encoding="utf-8")
        self._file_size = 0
        self._file_start_time = time.time()

    def _rotate_if_needed(self):
        if self._file is None:
            self._open_new()
            return
        now = time.time()
        if (self._file_size >= self.max_size_bytes or
                now - self._file_start_time >= self.rotate_interval_sec):
            self._file.close()
            self._cleanup_old()
            self._open_new()

    def _cleanup_old(self):
        try:
            files = sorted(
                [f for f in os.listdir(self.output_dir)
                 if f.startswith("ais_capture_") and f.endswith(".jsonl")],
                key=lambda f: os.path.getmtime(
                    os.path.join(self.output_dir, f)
                ),
            )
            while len(files) > self.max_files:
                oldest = files.pop(0)
                os.remove(os.path.join(self.output_dir, oldest))
        except Exception:
            pass

    def write(self, topic, payload):
        """写入一条原始消息。"""
        self._rotate_if_needed()
        rec = {
            "topic": topic,
            "receive_time_ms": int(time.time() * 1000),
            "payload": payload,
        }
        line = json.dumps(rec, ensure_ascii=False) + "\n"
        self._file.write(line)
        self._file.flush()
        self._file_size += len(line.encode("utf-8"))
        self._count += 1

    def close(self):
        if self._file:
            try:
                self._file.close()
            except Exception:
                pass
            self._file = None

    @property
    def count(self):
        return self._count
