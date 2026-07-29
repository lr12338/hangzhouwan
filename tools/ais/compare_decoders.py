#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""比较 pyais 和自研 AIS 解码器的一致性。

用法：
  python3 tools/ais/compare_decoders.py --file <ais_capture.jsonl>
  python3 tools/ais/compare_decoders.py --test  # 使用内置测试消息
"""
import argparse
import json
import os
import sys

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, _REPO_ROOT)

from services.business_enrichment.ais.decoder import PyAisDecoder, LightweightAisDecoder

# 内置测试消息（类型1/2/3/18/4）
TEST_PAYLOADS = [
    # 类型1位置报告
    ("!AIVDM,1,1,,A,15M67FC000G?ufbE`FepT@3n00Sa,0"),
    # 类型18 Class B
    ("!AIVDM,1,1,,A,B69>7m@0?j<:0PfBPhhqJwvb2HMv,0"),
    # 类型4基站
    ("!AIVDM,1,1,,A,403Owi1utO1pap@;16@6WQPTB1m0,0"),
]


def main():
    parser = argparse.ArgumentParser(description="AIS 解码器一致性比较")
    parser.add_argument("--file", type=str, default="", help="AIS 录制文件")
    parser.add_argument("--test", action="store_true", help="使用内置测试消息")
    args = parser.parse_args()

    py_decoder = PyAisDecoder()
    lw_decoder = LightweightAisDecoder()

    if not py_decoder._available:
        print("警告 | pyais 不可用，仅测试自研解码器", flush=True)

    payloads = []
    if args.test or not args.file:
        payloads = [(p, "test") for p in TEST_PAYLOADS]
        print(f"使用 {len(payloads)} 条内置测试消息", flush=True)
    else:
        with open(args.file, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                    payloads.append((rec.get("payload", ""), rec.get("topic", "")))
                except json.JSONDecodeError:
                    continue
        print(f"从文件加载 {len(payloads)} 条消息", flush=True)

    stats = {"total": 0, "pyais_ok": 0, "pyais_fail": 0,
             "lightweight_ok": 0, "lightweight_fail": 0,
             "agreement": 0, "disagreement": 0, "mismatches": []}

    for payload, topic in payloads:
        stats["total"] += 1
        py_result = py_decoder.decode(payload)
        lw_result = lw_decoder.decode(payload)

        if py_result:
            stats["pyais_ok"] += 1
        else:
            stats["pyais_fail"] += 1
        if lw_result:
            stats["lightweight_ok"] += 1
        else:
            stats["lightweight_fail"] += 1

        py_mmsi = py_result.get("mmsi") if py_result else None
        lw_mmsi = lw_result.get("mmsi") if lw_result else None
        if py_mmsi and lw_mmsi:
            if py_mmsi == lw_mmsi:
                stats["agreement"] += 1
            else:
                stats["disagreement"] += 1
                stats["mismatches"].append({
                    "payload": payload[:80],
                    "pyais_mmsi": py_mmsi,
                    "lightweight_mmsi": lw_mmsi,
                })
        elif py_mmsi and not lw_mmsi:
            stats["mismatches"].append({
                "payload": payload[:80],
                "pyais_mmsi": py_mmsi,
                "lightweight_mmsi": None,
                "issue": "lightweight failed to decode",
            })
        elif lw_mmsi and not py_mmsi:
            stats["mismatches"].append({
                "payload": payload[:80],
                "pyais_mmsi": None,
                "lightweight_mmsi": lw_mmsi,
                "issue": "pyais failed to decode",
            })

    print(json.dumps(stats, ensure_ascii=False, indent=2))
    passed = stats["disagreement"] == 0
    print(f"\n{'✅' if passed else '❌'} 解码器一致性{'通过' if passed else '未通过'}")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
