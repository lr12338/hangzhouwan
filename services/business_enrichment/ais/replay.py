# -*- coding: utf-8 -*-
"""AIS replay 工具：从 JSONL 录制文件回放 AIS 消息。

支持：解码、过期、乱序、重复、多船匹配、无船、Sidecar降级验证。
"""
import json
import os
import time
import threading

from .decoder import PyAisDecoder
from .store import AisStore


class AisReplayer:
    """从 JSONL 文件回放 AIS 消息到 AisStore。"""

    def __init__(self, store, replay_speed=1.0, loop=True):
        self.store = store
        self.replay_speed = replay_speed
        self.loop = loop
        self._stop = False
        self._thread = None
        self._decoder = PyAisDecoder()
        self.replayed_count = 0

    def replay_file(self, filepath, inject_to_store=True):
        """回放单个文件，返回解码统计。"""
        stats = {"total": 0, "decoded": 0, "failed": 0}
        if not os.path.exists(filepath):
            return stats
        with open(filepath, "r", encoding="utf-8") as f:
            prev_ts = None
            for line in f:
                if self._stop:
                    break
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    continue
                stats["total"] += 1
                payload = rec.get("payload", "").strip()
                recv_ms = rec.get("receive_time_ms", 0)
                # 按录制间隔回放
                if prev_ts is not None and self.replay_speed > 0:
                    gap = (recv_ms - prev_ts) / 1000.0 / self.replay_speed
                    if 0 < gap < 5:
                        time.sleep(gap)
                prev_ts = recv_ms
                decoded = self._decoder.decode(payload)
                if decoded is not None:
                    stats["decoded"] += 1
                    if inject_to_store:
                        decoded["source_topic"] = rec.get("topic", "")
                        self.store.update_from_decoded(decoded)
                else:
                    stats["failed"] += 1
                self.replayed_count += 1
        return stats

    def replay_in_background(self, filepath):
        """在后台线程回放。"""
        def _run():
            while not self._stop:
                self.replay_file(filepath)
                if not self.loop:
                    break
        self._thread = threading.Thread(target=_run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop = True
        if self._thread:
            self._thread.join(timeout=2)
