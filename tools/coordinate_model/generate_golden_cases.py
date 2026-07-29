#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成坐标模型黄金测试样本。

覆盖：真实检测框、小框、大框、图像四角、图像中心、禁区边界、
随机合法框、非法框。每个模型至少1000个样本。

用法：
  python3 tools/coordinate_model/generate_golden_cases.py
"""
import json
import os
import random
import sys

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, _REPO_ROOT)

REF_W = 2560
REF_H = 1440
OUTPUT_DIR = os.environ.get("GOLDEN_OUTPUT", "artifacts/coordinate-models")
SEED = 42


def gen_cases():
    random.seed(SEED)
    cases = []
    # 1. 真实检测框（典型大小）
    real_boxes = [
        (500, 600, 800, 900), (1000, 400, 1200, 600),
        (300, 200, 500, 400), (800, 800, 1100, 1100),
        (1500, 300, 1700, 500), (2000, 1000, 2200, 1200),
        (100, 100, 300, 300), (600, 500, 900, 700),
        (1200, 800, 1500, 1000), (1800, 400, 2100, 600),
    ]
    for b in real_boxes:
        for _ in range(10):
            jitter = random.randint(-20, 20)
            cases.append({"type": "real", "x1": b[0]+jitter, "y1": b[1]+jitter,
                          "x2": b[2]+jitter, "y2": b[3]+jitter})

    # 2. 小框 (20x20 ~ 60x60)
    for _ in range(120):
        x = random.randint(0, REF_W - 60)
        y = random.randint(0, REF_H - 60)
        s = random.randint(20, 60)
        cases.append({"type": "small", "x1": x, "y1": y, "x2": x+s, "y2": y+s})

    # 3. 大框 (300x300 ~ 800x800)
    for _ in range(120):
        x = random.randint(0, max(0, REF_W - 800))
        y = random.randint(0, max(0, REF_H - 800))
        w = random.randint(300, min(800, REF_W - x))
        h = random.randint(300, min(800, REF_H - y))
        cases.append({"type": "large", "x1": x, "y1": y, "x2": x+w, "y2": y+h})

    # 4. 图像四角
    corners = [(0, 0), (REF_W-100, 0), (0, REF_H-100), (REF_W-100, REF_H-100)]
    for cx, cy in corners:
        for _ in range(50):
            dx = random.randint(-20, 20)
            dy = random.randint(-20, 20)
            cases.append({"type": "corner", "x1": max(0, cx+dx), "y1": max(0, cy+dy),
                          "x2": max(10, cx+dx+100), "y2": max(10, cy+dy+100)})

    # 5. 图像中心
    for _ in range(100):
        cx, cy = REF_W // 2, REF_H // 2
        dx = random.randint(-200, 200)
        dy = random.randint(-200, 200)
        s = random.randint(50, 200)
        cases.append({"type": "center", "x1": cx+dx-s//2, "y1": cy+dy-s//2,
                      "x2": cx+dx+s//2, "y2": cy+dy+s//2})

    # 6. 禁区边界（图像边缘附近）
    for _ in range(100):
        edge = random.choice(["top", "bottom", "left", "right"])
        if edge == "top":
            cases.append({"type": "forbidden_edge", "x1": random.randint(0, REF_W-100),
                          "y1": 0, "x2": random.randint(100, REF_W), "y2": random.randint(10, 50)})
        elif edge == "bottom":
            cases.append({"type": "forbidden_edge", "x1": random.randint(0, REF_W-100),
                          "y1": REF_H-50, "x2": random.randint(100, REF_W), "y2": REF_H})
        elif edge == "left":
            cases.append({"type": "forbidden_edge", "x1": 0, "y1": random.randint(0, REF_H-100),
                          "x2": random.randint(10, 50), "y2": random.randint(100, REF_H)})
        else:
            cases.append({"type": "forbidden_edge", "x1": REF_W-50, "y1": random.randint(0, REF_H-100),
                          "x2": REF_W, "y2": random.randint(100, REF_H)})

    # 7. 随机合法框
    for _ in range(200):
        x1 = random.randint(0, REF_W - 10)
        y1 = random.randint(0, REF_H - 10)
        x2 = random.randint(x1 + 1, REF_W)
        y2 = random.randint(y1 + 1, REF_H)
        cases.append({"type": "random_valid", "x1": x1, "y1": y1, "x2": x2, "y2": y2})

    # 8. 非法框（x2<x1, 负值, 超出范围）
    for _ in range(100):
        case_type = random.choice(["inverted", "negative", "overflow", "zero"])
        if case_type == "inverted":
            cases.append({"type": "illegal", "x1": 500, "y1": 500, "x2": 300, "y2": 300})
        elif case_type == "negative":
            cases.append({"type": "illegal", "x1": -10, "y1": -10, "x2": 100, "y2": 100})
        elif case_type == "overflow":
            cases.append({"type": "illegal", "x1": REF_W+10, "y1": REF_H+10,
                          "x2": REF_W+100, "y2": REF_H+100})
        else:
            cases.append({"type": "illegal", "x1": 0, "y1": 0, "x2": 0, "y2": 0})

    return cases[:1000]  # 确保至少1000


def main():
    cases = gen_cases()
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    output = os.path.join(OUTPUT_DIR, "golden_cases.json")
    with open(output, "w", encoding="utf-8") as f:
        json.dump({
            "reference_width": REF_W,
            "reference_height": REF_H,
            "count": len(cases),
            "cases": cases,
        }, f, ensure_ascii=False, indent=2)
    print(f"生成 {len(cases)} 个黄金样本 -> {output}")
    # 统计类型分布
    from collections import Counter
    types = Counter(c["type"] for c in cases)
    for t, n in sorted(types.items()):
        print(f"  {t}: {n}")


if __name__ == "__main__":
    main()
