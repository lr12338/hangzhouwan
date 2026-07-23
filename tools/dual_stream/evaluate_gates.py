#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""G1–G4 门禁自动评估器。

从 forced_reconnect 原始日志和 pre-summary JSON 中解析数据，
自动评估四项门禁，生成 gate_results 和 vpu_heap_analysis，
更新 overall_rc / failure_reason，输出最终 summary JSON。

调用：
  python3 tools/dual_stream/evaluate_gates.py <log_dir> <pre_summary> <output_summary>

门禁定义：
  G1  RTSP 重连成功率 100%   decoder ok=100 fail=0 且真实 RTSP 已验证
  G2  最终 inflight=0        各阶段末轮 inflight=0
  G3  VPU 资源无单调增长     所有堆各检查点 delta <= 阈值，无单调增长
  G4  无非法释放/内存错误    无 invalid free / ENOMEM / gmem / BMVidDecSeqInit 失败
"""
import glob
import json
import os
import re
import sys

G3_THRESHOLD_MB = 10.0
G3_BASIS = (
    "pre-fix leak ~39.5MB/round (100 rounds ~3950MB); "
    "threshold +%.1fMB is ~0.25%% of pre-fix rate, "
    "allows measurement noise while catching regression" % G3_THRESHOLD_MB
)

G4_PATTERNS = [
    r"invalid free",
    r"ENOMEM",
    r"gmem",
    r"BMVidDecSeqInit.*(?:fail|error|err)",
]
G4_REGEX = re.compile("|".join(G4_PATTERNS), re.IGNORECASE)


def parse_vpu_heap_lines(raw_file):
    """解析 VPU_HEAP 行，返回 [{tag, heaps:[{id,used_mb,avail_mb}]}]。"""
    checkpoints = []
    try:
        with open(raw_file, "r", errors="replace") as f:
            for line in f:
                if "VPU_HEAP" not in line:
                    continue
                parts = line.split("|")
                if len(parts) < 3:
                    continue
                tag = parts[1].strip()
                heaps = []
                for m in re.finditer(
                    r"heap(\d+):used=([0-9.]+)MB/avail=([0-9.]+)MB", parts[2]
                ):
                    heaps.append(
                        {
                            "id": int(m.group(1)),
                            "used_mb": float(m.group(2)),
                            "avail_mb": float(m.group(3)),
                        }
                    )
                if heaps:
                    checkpoints.append({"tag": tag, "heaps": heaps})
    except OSError:
        pass
    return checkpoints


def evaluate_g1(phases, phases_ran):
    """G1: RTSP 重连成功率 100%。"""
    if not phases_ran:
        return {
            "gate": "G1",
            "status": "NOT_EVALUATED",
            "detail": "no phases executed (production guard or candidate validation rejected)",
        }

    decoder = next((p for p in phases if p.get("name") == "decoder"), None)
    sim = next(
        (p for p in phases if p.get("name") == "decoder_reopen_simulation"), None
    )

    detail_parts = []
    decoder_ok = False
    if decoder:
        ok = decoder.get("ok")
        fail = decoder.get("fail")
        detail_parts.append(f"decoder ok={ok} fail={fail}")
        decoder_ok = ok == 100 and fail == 0
    else:
        detail_parts.append("decoder phase not run")

    if sim:
        detail_parts.append(
            "decoder_reopen_simulation is file-source decoder reopen, NOT real RTSP"
        )
    detail_parts.append(
        "real RTSP disconnect/reconnect not tested in this environment"
    )
    detail_parts.append(
        "G1 BLOCKED: cannot claim RTSP 100x validation without real RTSP source"
    )

    # G1 requires BOTH decoder ok=100/fail=0 AND real RTSP verification.
    # Real RTSP is not available, so G1 is always NOT_PASSED.
    status = "NOT_PASSED"
    return {"gate": "G1", "status": status, "detail": "; ".join(detail_parts)}


def evaluate_g2(phases, phases_ran):
    """G2: 最终 inflight=0。"""
    if not phases_ran:
        return {
            "gate": "G2",
            "status": "NOT_EVALUATED",
            "detail": "no phases executed",
        }

    fail_parts = []
    ok_parts = []
    for p in phases:
        name = p.get("name", "?")
        inflight = p.get("inflight_end")
        if inflight is None or inflight == "null":
            continue
        try:
            val = int(inflight)
        except (ValueError, TypeError):
            continue
        if val == 0:
            ok_parts.append(name)
        else:
            fail_parts.append(f"{name} inflight={val}")

    if fail_parts:
        return {
            "gate": "G2",
            "status": "FAIL",
            "detail": "; ".join(fail_parts),
        }
    return {
        "gate": "G2",
        "status": "PASS",
        "detail": "all phases final inflight=0" if ok_parts else "no inflight data (non-reconnect phases)",
    }


def evaluate_g3(log_dir, phases_ran):
    """G3: VPU 资源无单调增长（所有堆）。"""
    if not phases_ran:
        return {
            "gate": "G3",
            "status": "NOT_EVALUATED",
            "detail": "no phases executed",
            "vpu_heap_analysis": [],
        }

    analysis = []
    gate_fail = False
    fail_parts = []

    for raw_file in sorted(
        glob.glob(os.path.join(log_dir, "forced_reconnect_*.raw"))
    ):
        phase = (
            os.path.basename(raw_file)
            .replace("forced_reconnect_", "")
            .replace(".raw", "")
        )
        checkpoints = parse_vpu_heap_lines(raw_file)
        if not checkpoints:
            continue

        heap_ids = set()
        for cp in checkpoints:
            for h in cp["heaps"]:
                heap_ids.add(h["id"])

        phase_heaps = []
        for hid in sorted(heap_ids):
            used_values = []
            for cp in checkpoints:
                for h in cp["heaps"]:
                    if h["id"] == hid:
                        used_values.append(h["used_mb"])
                        break

            if len(used_values) < 2:
                continue

            start = used_values[0]
            final = used_values[-1]
            delta = round(final - start, 2)

            increases = sum(
                1
                for i in range(1, len(used_values))
                if used_values[i] > used_values[i - 1]
            )
            monotonic = increases > len(used_values) / 2 and delta > 0

            phase_heaps.append(
                {
                    "id": hid,
                    "start_mb": start,
                    "final_mb": final,
                    "delta_mb": delta,
                    "monotonic_growth": monotonic,
                    "checkpoint_count": len(used_values),
                }
            )

            if delta > G3_THRESHOLD_MB or monotonic:
                gate_fail = True
                fail_parts.append(
                    f"{phase} heap{hid} delta=+{delta:.1f}MB monotonic={monotonic}"
                )

        if phase_heaps:
            analysis.append({"phase": phase, "heaps": phase_heaps})

    if gate_fail:
        detail = "; ".join(fail_parts) + f"; threshold=+{G3_THRESHOLD_MB}MB ({G3_BASIS})"
        status = "FAIL"
    else:
        detail = f"all heaps delta <= {G3_THRESHOLD_MB}MB, no monotonic growth; threshold=+{G3_THRESHOLD_MB}MB ({G3_BASIS})"
        status = "PASS"

    return {
        "gate": "G3",
        "status": status,
        "detail": detail,
        "vpu_heap_analysis": analysis,
    }


def evaluate_g4(log_dir, phases_ran):
    """G4: 无非法释放/内存错误。"""
    if not phases_ran:
        return {
            "gate": "G4",
            "status": "NOT_EVALUATED",
            "detail": "no phases executed",
            "error_matches": [],
        }

    matches = []
    for raw_file in sorted(
        glob.glob(os.path.join(log_dir, "forced_reconnect_*.raw"))
    ):
        phase = (
            os.path.basename(raw_file)
            .replace("forced_reconnect_", "")
            .replace(".raw", "")
        )
        try:
            with open(raw_file, "r", errors="replace") as f:
                for lineno, line in enumerate(f, 1):
                    if G4_REGEX.search(line):
                        matches.append(
                            {
                                "phase": phase,
                                "line": lineno,
                                "text": line.strip()[:200],
                            }
                        )
        except OSError:
            pass

    if matches:
        return {
            "gate": "G4",
            "status": "FAIL",
            "detail": f"{len(matches)} error pattern matches (invalid free/ENOMEM/gmem/BMVidDecSeqInit)",
            "error_matches": matches[:20],
        }
    return {
        "gate": "G4",
        "status": "PASS",
        "detail": "no invalid free/ENOMEM/gmem/BMVidDecSeqInit failures",
        "error_matches": [],
    }


def main():
    if len(sys.argv) < 4:
        print(
            "用法: evaluate_gates.py <log_dir> <pre_summary> <output_summary>",
            file=sys.stderr,
        )
        sys.exit(2)

    log_dir = sys.argv[1]
    pre_summary_path = sys.argv[2]
    output_path = sys.argv[3]

    with open(pre_summary_path, "r", encoding="utf-8") as f:
        summary = json.load(f)

    phases = summary.get("phases", [])
    phases_ran = len(phases) > 0

    gates = [
        evaluate_g1(phases, phases_ran),
        evaluate_g2(phases, phases_ran),
        evaluate_g3(log_dir, phases_ran),
        evaluate_g4(log_dir, phases_ran),
    ]

    gate_failed = any(g["status"] in ("FAIL", "NOT_PASSED") for g in gates)
    if gate_failed and summary.get("overall_rc", 0) == 0:
        summary["overall_rc"] = 8
        summary["overall"] = "FAIL"
        if not summary.get("failure_reason"):
            failed = [g["gate"] for g in gates if g["status"] in ("FAIL", "NOT_PASSED")]
            summary["failure_reason"] = "gate_failed:" + ",".join(failed)

    summary["gate_results"] = gates

    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, ensure_ascii=False)
        f.write("\n")

    for g in gates:
        print(f"{g['gate']}: {g['status']} - {g['detail']}", file=sys.stderr)


if __name__ == "__main__":
    main()
