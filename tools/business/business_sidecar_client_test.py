#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""业务 Sidecar 客户端测试：启动 sidecar，发送模拟请求，验证响应。

测试项：
  1. 坐标预测（真实模型或 numpy 向量化）
  2. AIS 解码（类型 1/4/18）
  3. AIS 缓存与匹配
  4. MQTT 消息端到端（可选，需 MQTT 配置）
"""
import json
import os
import socket
import subprocess
import sys
import time

SOCKET_PATH = "/tmp/hangzhouwan-business-test.sock"


def test_coordinate_prediction(sock):
    """测试坐标预测。"""
    req = {
        "stream_id": "A",
        "frame_sequence": 1,
        "image_width": 2560,
        "image_height": 1440,
        "detections": [
            {"detection_id": 0, "score": 0.85, "x1": 500, "y1": 600, "x2": 800, "y2": 900},
            {"detection_id": 1, "score": 0.72, "x1": 1000, "y1": 400, "x2": 1200, "y2": 600},
        ]
    }
    sock.sendall((json.dumps(req) + "\n").encode())
    data = b""
    while b"\n" not in data:
        data += sock.recv(4096)
    resp = json.loads(data.strip())
    assert resp["stream_id"] == "A"
    assert len(resp["results"]) == 2
    for r in resp["results"]:
        assert r["coordinate_valid"], f"坐标无效: {r}"
        assert 120.0 < r["longitude"] < 122.0, f"经度超出杭州湾范围: {r['longitude']}"
        assert 30.0 < r["latitude"] < 31.0, f"纬度超出杭州湾范围: {r['latitude']}"
    print(f"  坐标预测通过: {resp['results'][0]['longitude']:.6f}, {resp['results'][0]['latitude']:.6f}")
    return resp


def test_b_coordinate(sock):
    """测试 B 路坐标预测（中心点特征）。"""
    req = {
        "stream_id": "B",
        "frame_sequence": 2,
        "image_width": 2560,
        "image_height": 1440,
        "detections": [
            {"detection_id": 0, "score": 0.9, "x1": 1100, "y1": 500, "x2": 1400, "y2": 800},
        ]
    }
    sock.sendall((json.dumps(req) + "\n").encode())
    data = b""
    while b"\n" not in data:
        data += sock.recv(4096)
    resp = json.loads(data.strip())
    assert resp["stream_id"] == "B"
    assert len(resp["results"]) == 1
    r = resp["results"][0]
    assert r["coordinate_valid"]
    assert 120.0 < r["longitude"] < 122.0
    assert 30.0 < r["latitude"] < 31.0
    print(f"  B路坐标预测通过: {r['longitude']:.6f}, {r['latitude']:.6f}")


def test_ais_decoding():
    """测试 AIS 6-bit 解码。"""
    sys.path.insert(0, "tools/business")
    from business_sidecar import parse_ais_payload, haversine_km, AisStore, match_detections_to_ais

    # Type 1
    r1 = parse_ais_payload("15M67FC000G?ufbE`FepT@3n00Sa")
    assert r1 and r1["msg_type"] == 1
    assert "lon" in r1 and "lat" in r1
    print(f"  类型1解码通过: mmsi={r1['mmsi']} lon={r1['lon']:.4f} lat={r1['lat']:.4f}")

    # Type 18
    r18 = parse_ais_payload("B69>7m@0?j<:0PfBPhhqJwvb2HMv")
    assert r18 and r18["msg_type"] == 18
    print(f"  类型18解码通过: mmsi={r18['mmsi']}")

    # Type 4
    r4 = parse_ais_payload("403Owi1udPPPEOdgQH`1L`4R0H3k")
    assert r4 and r4["msg_type"] == 4
    print(f"  类型4解码通过: mmsi={r4['mmsi']}")

    # Haversine
    d = haversine_km(121.05, 30.56, 121.06, 30.57)
    assert 1.0 < d < 2.0
    print(f"  Haversine距离通过: {d:.4f}km")

    # AIS matching
    store = AisStore(max_capacity=100, timeout_sec=300)
    store.update("123456789", 121.055, 30.569, 5.0, 180.0)
    store.update("987654321", 121.10, 30.60, 3.0, 90.0)
    snap = store.snapshot()
    dets = [
        {"detection_id": 0, "longitude": 121.0548, "latitude": 30.5688, "coordinate_valid": True},
        {"detection_id": 1, "longitude": 121.09, "latitude": 30.58, "coordinate_valid": True},
    ]
    matches = match_detections_to_ais(dets, snap, max_distance_km=0.5)
    assert 0 in matches, "检测框0应该匹配AIS"
    assert matches[0]["mmsi"] == "123456789"
    assert 1 not in matches, "检测框1不应匹配（距离过远）"
    print(f"  AIS匹配通过: det0->mmsi={matches[0]['mmsi']} dist={matches[0]['ais_distance_km']}km")

    # 一对一匹配验证
    store2 = AisStore(max_capacity=100, timeout_sec=300)
    store2.update("111", 121.055, 30.569, 1.0, 0.0)
    store2.update("222", 121.056, 30.570, 2.0, 0.0)
    snap2 = store2.snapshot()
    dets2 = [
        {"detection_id": 0, "longitude": 121.055, "latitude": 30.569, "coordinate_valid": True},
        {"detection_id": 1, "longitude": 121.056, "latitude": 30.570, "coordinate_valid": True},
    ]
    matches2 = match_detections_to_ais(dets2, snap2, max_distance_km=0.5)
    assert len(matches2) == 2, f"应匹配2个，实际{len(matches2)}"
    matched_mmsi = {m["mmsi"] for m in matches2.values()}
    assert matched_mmsi == {"111", "222"}, f"MMSI不匹配: {matched_mmsi}"
    print(f"  一对一匹配通过: {matched_mmsi}")


def main():
    env = os.environ.copy()
    env["HANGZHOUWAN_BUSINESS_SOCK"] = SOCKET_PATH
    env["COORD_MODEL_A"] = "weights/0121_random_forest_model.pkl"
    env["COORD_MODEL_B"] = "weights/beishang_x-l.pkl"
    env.pop("AIS_MQTT_HOST", None)  # 禁用 MQTT 避免重试

    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)
    proc = subprocess.Popen(
        [sys.executable, "tools/business/business_sidecar.py"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    for _ in range(50):
        if os.path.exists(SOCKET_PATH):
            break
        time.sleep(0.1)
    else:
        print("ERROR: Sidecar 未启动")
        proc.kill()
        return 1

    time.sleep(1)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect(SOCKET_PATH)

    print("测试 1: AIS 解码与匹配")
    test_ais_decoding()

    print("测试 2: 坐标预测（真实模型）")
    test_coordinate_prediction(sock)

    print("测试 3: B 路坐标预测")
    test_b_coordinate(sock)

    sock.close()
    proc.terminate()
    proc.wait()

    print("\n=== 全部测试通过 ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
