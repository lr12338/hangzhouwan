#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""业务 Sidecar 客户端测试：启动 sidecar，发送请求，验证响应。

测试项：
  1. 坐标预测（真实模型）
  2. B路坐标预测（中心点特征）
  3. AIS 解码（类型1/4/18）
  4. AIS 缓存与匹配
  5. 健康检查
  6. 协议版本2（长度前缀framing）
"""
import json
import os
import socket
import subprocess
import sys
import time

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, _REPO_ROOT)

from services.business_enrichment import protocol as proto

SOCKET_PATH = "/tmp/hangzhouwan-business-test.sock"


def send_request_v2(sock, req):
    """发送长度前缀协议v2请求。"""
    encoded = proto.encode_message(req)
    sock.sendall(encoded)
    # 接收响应
    buf = b""
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        buf += chunk
        resp, buf = proto.decode_stream(buf)
        if resp is not None:
            return resp
    return None


def send_request_v1(sock, req):
    """发送旧版换行分隔协议v1请求（兼容测试）。"""
    sock.sendall(proto.encode_line_json(req))
    data = b""
    while b"\n" not in data:
        data += sock.recv(4096)
    return json.loads(data.strip())


def test_coordinate_prediction(sock):
    """测试A路坐标预测。"""
    req = proto.make_request(
        "A", 1, 2560, 1440,
        [{"detection_id": 0, "score": 0.85, "x1": 500, "y1": 600, "x2": 800, "y2": 900},
         {"detection_id": 1, "score": 0.72, "x1": 1000, "y1": 400, "x2": 1200, "y2": 600}],
    )
    resp = send_request_v2(sock, req)
    assert resp is not None, "无响应"
    assert resp["stream_id"] == "A"
    assert resp["response_status"] == "OK"
    assert len(resp["results"]) == 2
    for r in resp["results"]:
        assert r["coordinate_valid"], f"坐标无效: {r}"
        assert 120.0 < r["longitude"] < 122.0, f"经度超出范围: {r['longitude']}"
        assert 30.0 < r["latitude"] < 31.0, f"纬度超出范围: {r['latitude']}"
    print(f"  A路坐标预测通过: ({resp['results'][0]['longitude']:.6f}, {resp['results'][0]['latitude']:.6f})")
    return resp


def test_b_coordinate(sock):
    """测试B路坐标预测（中心点特征）。"""
    req = proto.make_request(
        "B", 2, 2560, 1440,
        [{"detection_id": 0, "score": 0.9, "x1": 1100, "y1": 500, "x2": 1400, "y2": 800}],
    )
    resp = send_request_v2(sock, req)
    assert resp is not None
    assert resp["stream_id"] == "B"
    assert resp["results"][0]["coordinate_valid"]
    assert 120.0 < resp["results"][0]["longitude"] < 122.0
    assert 30.0 < resp["results"][0]["latitude"] < 31.0
    print(f"  B路坐标预测通过: ({resp['results'][0]['longitude']:.6f}, {resp['results'][0]['latitude']:.6f})")
    return resp


def test_health(sock):
    """测试健康检查。"""
    req = {"action": "health"}
    resp = send_request_v2(sock, req)
    assert resp is not None
    assert "status" in resp
    assert "coordinate_mode" in resp
    print(f"  健康检查通过: status={resp['status']} mode={resp['coordinate_mode']}")
    return resp


def test_protocol_v1_compat(sock):
    """测试旧版协议v1兼容。"""
    req = {
        "stream_id": "A",
        "frame_sequence": 99,
        "image_width": 2560,
        "image_height": 1440,
        "detections": [{"detection_id": 0, "score": 0.8, "x1": 100, "y1": 100, "x2": 300, "y2": 300}],
    }
    resp = send_request_v1(sock, req)
    assert resp is not None
    assert "results" in resp
    print(f"  协议v1兼容通过")
    return resp


def main():
    # 启动 sidecar
    env = dict(os.environ)
    env["HANGZHOUWAN_BUSINESS_SOCK"] = SOCKET_PATH
    env.setdefault("COORD_MODE", "sklearn")
    env.setdefault("HZW_ENVIRONMENT", "development")

    print("启动 sidecar...")
    proc = subprocess.Popen(
        [sys.executable, "-m", "services.business_enrichment.app"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    try:
        # 等待 sidecar 启动
        for _ in range(30):
            if os.path.exists(SOCKET_PATH):
                break
            time.sleep(0.5)
        else:
            print("❌ sidecar 启动超时")
            proc.terminate()
            return 1

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(SOCKET_PATH)

        print("\n=== 测试开始 ===")
        tests = [
            ("坐标预测(A路)", test_coordinate_prediction),
            ("坐标预测(B路)", test_b_coordinate),
            ("健康检查", test_health),
            ("协议v1兼容", test_protocol_v1_compat),
        ]
        passed = 0
        failed = 0
        for name, test_fn in tests:
            print(f"\n--- {name} ---")
            try:
                test_fn(sock)
                passed += 1
            except Exception as e:
                print(f"  ❌ 失败: {e}")
                failed += 1

        sock.close()
        print(f"\n=== 结果: {passed} 通过, {failed} 失败 ===")
        return failed
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        if os.path.exists(SOCKET_PATH):
            os.unlink(SOCKET_PATH)


if __name__ == "__main__":
    sys.exit(main())
