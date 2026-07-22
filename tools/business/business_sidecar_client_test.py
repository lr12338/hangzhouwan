#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""业务 Sidecar 客户端测试：启动 sidecar，发送模拟请求，验证响应。"""
import json
import os
import socket
import subprocess
import sys
import time

SOCKET_PATH = "/tmp/hangzhouwan-business-test.sock"

def main():
    env = os.environ.copy()
    env["HANGZHOUWAN_BUSINESS_SOCK"] = SOCKET_PATH
    env["COORD_MODEL_A"] = "weights/0121_random_forest_model.pkl"
    env["COORD_MODEL_B"] = "weights/beishang_x-l.pkl"

    # 启动 sidecar
    proc = subprocess.Popen(
        [sys.executable, "tools/business/business_sidecar.py"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    # 等待 sidecar 就绪
    for _ in range(50):
        if os.path.exists(SOCKET_PATH):
            break
        time.sleep(0.1)
    else:
        print("ERROR: Sidecar 未启动")
        proc.kill()
        return 1

    time.sleep(0.5)  # 等待 MQTT 连接

    # 发送测试请求
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    sock.connect(SOCKET_PATH)

    req = {
        "stream_id": "A",
        "frame_sequence": 1,
        "frame_time_ms": 0,
        "image_width": 2560,
        "image_height": 1440,
        "detections": [
            {"detection_id": 0, "score": 0.85, "x1": 100, "y1": 200, "x2": 300, "y2": 400},
            {"detection_id": 1, "score": 0.72, "x1": 500, "y1": 600, "x2": 700, "y2": 800},
        ]
    }
    sock.sendall((json.dumps(req) + "\n").encode())
    resp_data = b""
    while b"\n" not in resp_data:
        resp_data += sock.recv(4096)
    sock.close()

    resp = json.loads(resp_data.strip())
    print(f"stream_id={resp['stream_id']} frame={resp['frame_sequence']} "
          f"processing_ms={resp['processing_ms']}")
    for r in resp["results"]:
        print(f"  det={r['detection_id']} lon={r['longitude']} lat={r['latitude']} "
              f"valid={r['coordinate_valid']} ais_matched={r['ais_matched']}")

    # 验证
    assert resp["stream_id"] == "A"
    assert len(resp["results"]) == 2
    assert all(r["coordinate_valid"] for r in resp["results"])
    print("\n测试通过：接口模拟模式坐标预测正常")

    proc.terminate()
    proc.wait()
    return 0

if __name__ == "__main__":
    sys.exit(main())
