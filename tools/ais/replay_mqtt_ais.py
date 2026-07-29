#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""AIS replay 工具：从录制文件回放 AIS 消息到 sidecar 或直接解码验证。

用法：
  python3 tools/ais/replay_mqtt_ais.py --file artifacts/internal-development/ais_capture/ais_capture_*.jsonl
  python3 tools/ais/replay_mqtt_ais.py --file <file> --speed 10 --loop
"""
import argparse
import json
import os
import sys
import time

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, _REPO_ROOT)

from services.business_enrichment.ais.decoder import PyAisDecoder, LightweightAisDecoder
from services.business_enrichment.ais.store import AisStore


def main():
    parser = argparse.ArgumentParser(description="AIS replay 工具")
    parser.add_argument("--file", type=str, required=True, help="录制 JSONL 文件路径")
    parser.add_argument("--speed", type=float, default=1.0, help="回放速度倍率")
    parser.add_argument("--loop", action="store_true", help="循环回放")
    parser.add_argument("--decode-only", action="store_true", help="仅解码统计，不写入store")
    parser.add_argument("--compare", action="store_true", help="比较 pyais 和自研解码器")
    args = parser.parse_args()

    if not os.path.exists(args.file):
        print(f"错误 | 文件不存在: {args.file}", flush=True)
        sys.exit(1)

    py_decoder = PyAisDecoder()
    lw_decoder = LightweightAisDecoder()
    store = AisStore(max_capacity=500, timeout_sec=30) if not args.decode_only else None

    stats = {"total": 0, "pyais_ok": 0, "pyais_fail": 0,
             "lightweight_ok": 0, "lightweight_fail": 0,
             "agreement": 0, "disagreement": 0}

    while True:
        with open(args.file, "r", encoding="utf-8") as f:
            prev_ts = None
            for line in f:
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
                if prev_ts is not None and args.speed > 0:
                    gap = (recv_ms - prev_ts) / 1000.0 / args.speed
                    if 0 < gap < 5:
                        time.sleep(gap)
                prev_ts = recv_ms

                py_result = py_decoder.decode(payload)
                lw_result = lw_decoder.decode(payload)

                if py_result:
                    stats["pyais_ok"] += 1
                    if store:
                        store.update_from_decoded(py_result)
                else:
                    stats["pyais_fail"] += 1

                if lw_result:
                    stats["lightweight_ok"] += 1
                else:
                    stats["lightweight_fail"] += 1

                if args.compare:
                    py_mmsi = py_result.get("mmsi") if py_result else None
                    lw_mmsi = lw_result.get("mmsi") if lw_result else None
                    if py_mmsi and lw_mmsi:
                        if py_mmsi == lw_mmsi:
                            stats["agreement"] += 1
                        else:
                            stats["disagreement"] += 1

        print(json.dumps(stats, ensure_ascii=False, indent=2), flush=True)
        if not args.loop:
            break
        print("循环回放...", flush=True)

    if store:
        print(f"缓存船数: {store.count()}", flush=True)


if __name__ == "__main__":
    main()
