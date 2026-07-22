#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MQTT AIS 原始消息录制工具。

录制 JSONL 格式：
  {"topic": "", "receive_time_ms": 0, "payload": ""}

用法：
  python3 tools/ais/capture_mqtt_ais.py --seconds 300 --output artifacts/internal-development/ais_capture/
"""
import argparse
import json
import os
import sys
import time

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, _REPO_ROOT)

from services.business_enrichment.ais.capture import AisCapture


def main():
    parser = argparse.ArgumentParser(description="MQTT AIS 原始消息录制")
    parser.add_argument("--seconds", type=int, default=300, help="录制时长（秒）")
    parser.add_argument("--output", type=str, default="artifacts/internal-development/ais_capture",
                        help="输出目录")
    parser.add_argument("--max-size-mb", type=int, default=50, help="单个文件最大MB")
    parser.add_argument("--max-files", type=int, default=10, help="最大保留文件数")
    args = parser.parse_args()

    host = os.environ.get("AIS_MQTT_HOST", "")
    port = int(os.environ.get("AIS_MQTT_PORT", "1883"))
    client_id = os.environ.get("AIS_MQTT_CLIENT_ID", "")
    username = os.environ.get("AIS_MQTT_USERNAME", "")
    password = os.environ.get("AIS_MQTT_PASSWORD", "")
    topics_str = os.environ.get("AIS_MQTT_TOPICS", "upAIS/base_2250,upAIS/base_2251")
    topics = [t.strip() for t in topics_str.split(",") if t.strip()]

    if not host:
        print("错误 | 未配置 AIS_MQTT_HOST", flush=True)
        sys.exit(1)

    capture = AisCapture(args.output, args.max_size_mb, args.max_files)

    import paho.mqtt.client as mqtt

    def on_connect(client, userdata, flags, rc, properties=None):
        if rc == 0:
            print(f"信息 | MQTT | 连接成功 {host}:{port}", flush=True)
            for t in topics:
                client.subscribe(t)
                print(f"信息 | MQTT | 订阅 {t}", flush=True)
        else:
            print(f"错误 | MQTT | 连接失败 rc={rc}", flush=True)

    def on_message(client, userdata, msg):
        raw = msg.payload.decode("utf-8", errors="replace").strip()
        capture.write(msg.topic, raw)

    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=client_id,
    )
    if username:
        client.username_pw_set(username, password)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(host, port, 60)

    print(f"信息 | 录制 {args.seconds}秒...", flush=True)
    start = time.time()
    while time.time() - start < args.seconds:
        client.loop(timeout=1.0)
    client.disconnect()
    capture.close()
    print(f"信息 | 录制完成，共 {capture.count} 条消息", flush=True)


if __name__ == "__main__":
    main()
