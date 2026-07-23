# -*- coding: utf-8 -*-
"""MQTT AIS 订阅器，线程安全连接与自动重连。"""
import threading
import time
import re

from .decoder import PyAisDecoder

# 追加在 AIVDM 句末的时间戳后缀：*<10位数字>
_TS_SUFFIX_RE = re.compile(r"\*(\d{10})$")
# 多分片重组等待中哨兵（不计入解析统计）
_FRAG_WAITING = object()


class MqttAisSubscriber:
    """MQTT AIS 订阅器。

    通过 paho-mqtt 连接 broker，订阅 AIS 主题，
    将消息解码后写入 AisStore。
    """

    def __init__(self, store, host, port=1883, client_id="", username="",
                 password="", topics=None, keepalive=60, reconnect_sec=5,
                 enable_capture=False, capture_dir=None):
        self.store = store
        self.host = host
        self.port = port
        self.client_id = client_id
        self.username = username
        self.password = password
        self.topics = topics or []
        self.keepalive = keepalive
        self.reconnect_sec = reconnect_sec
        self.enable_capture = enable_capture
        self.capture_dir = capture_dir
        self._stop = False
        self.connected = False
        self.reconnect_count = 0
        self.last_message_time = 0.0
        self.client = None
        self._decoder = PyAisDecoder()
        self._capture_file = None
        self._capture_count = 0
        self._lock = threading.Lock()
        self._topic_counts = {}
        self._fragments = {}  # (seq_id, channel) -> {total, parts, first_ts}
        self._frag_lock = threading.Lock()

    def _on_connect(self, client, userdata, flags, rc, properties=None):
        if rc == 0:
            self.connected = True
            self.client = client
            for t in self.topics:
                client.subscribe(t)
        else:
            self.connected = False

    def _on_disconnect(self, client, userdata, rc, properties=None):
        self.connected = False
        if not self._stop:
            self.reconnect_count += 1

    def _on_message(self, client, userdata, msg):
        try:
            raw = msg.payload.decode("utf-8", errors="replace").strip()
            self.last_message_time = time.time()
            with self._lock:
                self._topic_counts[msg.topic] = self._topic_counts.get(msg.topic, 0) + 1
            if self.enable_capture and self._capture_file:
                import json
                rec = {
                    "topic": msg.topic,
                    "receive_time_ms": int(self.last_message_time * 1000),
                    "payload": raw,
                }
                self._capture_file.write(
                    json.dumps(rec, ensure_ascii=False) + "\n"
                )
                self._capture_file.flush()
                self._capture_count += 1
            decoded = self._decode_with_reassembly(raw)
            if decoded is _FRAG_WAITING:
                return  # 多分片未齐，不计入统计
            self.store.update_from_decoded(decoded)
        except Exception:
            with self._lock:
                self.store.parse_fail += 1

    def _decode_with_reassembly(self, raw):
        """解码单条消息，支持多分片 AIVDM 重组。

        - 单分片或非 AIVDM：直接解码。
        - 多分片：按 (seq_id, channel) 缓冲，全部分片到齐后拼接 payload 解码。
        - 分片未齐：返回 _FRAG_WAITING（不计入解析统计）。
        """
        stripped = raw.strip()
        if not stripped.startswith("!"):
            return self._decoder.decode(raw)
        # 去掉追加的时间戳后缀后解析 NMEA 头
        ts_stripped = _TS_SUFFIX_RE.sub("", stripped).strip()
        fields = ts_stripped.split(",")
        if len(fields) < 6:
            return self._decoder.decode(raw)
        try:
            total = int(fields[1])
        except (ValueError, TypeError):
            return self._decoder.decode(raw)
        if total <= 1:
            return self._decoder.decode(raw)
        try:
            frag = int(fields[2])
        except (ValueError, TypeError):
            return self._decoder.decode(raw)
        seq_id = fields[3] if len(fields) > 3 and fields[3] else ""
        channel = fields[4] if len(fields) > 4 and fields[4] else ""
        payload = fields[5]
        key = (seq_id, channel)
        now = time.time()
        with self._frag_lock:
            # 清理超过 30 秒仍未齐的分片缓冲
            if self._fragments:
                stale = [k for k, v in self._fragments.items()
                         if now - v["first_ts"] > 30]
                for k in stale:
                    del self._fragments[k]
            entry = self._fragments.get(key)
            if entry is None:
                entry = {"total": total, "parts": {}, "first_ts": now}
                self._fragments[key] = entry
            entry["parts"][frag] = payload
            if len(entry["parts"]) < total:
                return _FRAG_WAITING
            combined = "".join(entry["parts"][i]
                               for i in range(1, total + 1)
                               if i in entry["parts"])
            del self._fragments[key]
        sentence = "!AIVDM,1,1,,A," + combined + ",0"
        return self._decoder.decode(sentence)

    def _open_capture(self):
        if not self.enable_capture or not self.capture_dir:
            return
        import os
        os.makedirs(self.capture_dir, exist_ok=True)
        ts = int(time.time())
        path = os.path.join(self.capture_dir, f"ais_capture_{ts}.jsonl")
        self._capture_file = open(path, "w", encoding="utf-8")

    def _close_capture(self):
        if self._capture_file:
            try:
                self._capture_file.close()
            except Exception:
                pass
            self._capture_file = None

    def run(self):
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            return
        self._open_capture()
        while not self._stop:
            try:
                client = mqtt.Client(
                    callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
                    client_id=self.client_id,
                )
                if self.username:
                    client.username_pw_set(self.username, self.password)
                client.on_connect = self._on_connect
                client.on_disconnect = self._on_disconnect
                client.on_message = self._on_message
                client.connect(self.host, self.port, self.keepalive)
                client.loop_forever(retry_first_connection=True)
            except Exception:
                if not self._stop:
                    self.reconnect_count += 1
                    time.sleep(self.reconnect_sec)
        self._close_capture()

    def stop(self):
        self._stop = True
        if self.client:
            try:
                self.client.disconnect()
            except Exception:
                pass
        self._close_capture()

    def stats(self):
        with self._lock:
            return {
                "connected": self.connected,
                "reconnect_count": self.reconnect_count,
                "last_message_time": self.last_message_time,
                "topic_counts": dict(self._topic_counts),
                "capture_count": self._capture_count,
            }
