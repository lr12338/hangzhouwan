#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
杭州湾双路船舶检测 · 业务 Sidecar（阶段4.4）

职责：
  1. 启动时加载 A/B 坐标模型（sklearn 1.3.2，原生 joblib.load）；
  2. 连接 MQTT 订阅 AIS 数据（paho-mqtt），缓存并定时清理过期记录；
  3. 通过 Unix Domain Socket 接收 C++ 批量请求：
     - 坐标预测（A 用完整框，B 用中心点）
     - AIS 最近邻匹配（haversine 距离，一对一）
  4. Sidecar 异常或超时不阻塞视频热路径。

协议：JSON over Unix Domain Socket，请求/响应各一行。
"""
import json
import math
import os
import re
import socket
import sys
import threading
import time

import numpy as np

SOCKET_PATH = os.environ.get(
    "HANGZHOUWAN_BUSINESS_SOCK",
    "/tmp/hangzhouwan-business.sock"
)

EARTH_RADIUS_KM = 6371.0

# ===== AIS 6-bit 解码 =====
def _decode_6bit(text):
    bits = []
    for c in text:
        v = ord(c)
        if v < 88:
            val = v - 48
        else:
            val = v - 56
        val &= 0x3F
        bits.extend([(val >> (5 - i)) & 1 for i in range(6)])
    return bits

def _bits_to_uint(bits, start, length):
    val = 0
    for i in range(length):
        val = (val << 1) | bits[start + i]
    return val

def _bits_to_int(bits, start, length):
    val = _bits_to_uint(bits, start, length)
    if val & (1 << (length - 1)):
        val -= (1 << length)
    return val

def parse_ais_payload(payload_str):
    """解析 AIVDM payload，返回 dict 或 None。"""
    try:
        bits = _decode_6bit(payload_str)
        if len(bits) < 38:
            return None
        msg_type = _bits_to_uint(bits, 0, 6)
        mmsi = _bits_to_uint(bits, 8, 30)
        result = {"msg_type": msg_type, "mmsi": str(mmsi)}
        if msg_type in (1, 2, 3):
            if len(bits) < 128:
                return None
            speed = _bits_to_uint(bits, 50, 10) / 10.0
            lon = _bits_to_int(bits, 61, 28) / 600000.0
            lat = _bits_to_int(bits, 89, 27) / 600000.0
            course = _bits_to_uint(bits, 116, 12) / 10.0
            result.update({"lon": lon, "lat": lat, "speed": speed, "course": course})
        elif msg_type == 18:
            if len(bits) < 128:
                return None
            speed = _bits_to_uint(bits, 50, 10) / 10.0
            lon = _bits_to_int(bits, 61, 28) / 600000.0
            lat = _bits_to_int(bits, 89, 27) / 600000.0
            course = _bits_to_uint(bits, 116, 12) / 10.0
            result.update({"lon": lon, "lat": lat, "speed": speed, "course": course})
        elif msg_type == 4:
            if len(bits) < 128:
                return None
            lon = _bits_to_int(bits, 79, 28) / 600000.0
            lat = _bits_to_int(bits, 107, 27) / 600000.0
            result.update({"lon": lon, "lat": lat, "speed": 0.0, "course": 0.0})
        else:
            return None
        if "lon" in result and "lat" in result:
            if not (-180 <= result["lon"] <= 180) or not (-90 <= result["lat"] <= 90):
                return None
            if result["lon"] == 0 and result["lat"] == 0:
                return None
        return result
    except Exception:
        return None


def haversine_km(lon1, lat1, lon2, lat2):
    lon1, lat1, lon2, lat2 = map(math.radians, [lon1, lat1, lon2, lat2])
    dlon = lon2 - lon1
    dlat = lat2 - lat1
    a = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
    return EARTH_RADIUS_KM * c


class AisStore:
    """线程安全、容量受控的 AIS 缓存。"""
    def __init__(self, max_capacity=500, timeout_sec=30):
        self._lock = threading.Lock()
        self._data = {}
        self._max = max_capacity
        self._timeout = timeout_sec
        self.msg_count = 0
        self.parse_ok = 0
        self.parse_fail = 0

    def update(self, mmsi, lon, lat, speed, course, timestamp=None):
        with self._lock:
            if len(self._data) >= self._max and mmsi not in self._data:
                oldest = min(self._data, key=lambda k: self._data[k].get("recv_ts", 0))
                del self._data[oldest]
            self._data[mmsi] = {
                "mmsi": str(mmsi),
                "lon": lon, "lat": lat,
                "speed": speed, "course": course,
                "msg_ts": timestamp or time.time(),
                "recv_ts": time.time(),
            }

    def snapshot(self):
        with self._lock:
            now = time.time()
            return {k: v for k, v in self._data.items()
                    if now - v["recv_ts"] < self._timeout}

    def cleanup(self):
        with self._lock:
            now = time.time()
            expired = [k for k, v in self._data.items()
                       if now - v["recv_ts"] >= self._timeout]
            for k in expired:
                del self._data[k]
            return len(expired)

    def count(self):
        with self._lock:
            return len(self._data)


class NumpyRandomForest:
    """纯 numpy 随机森林预测器：向量化遍历所有树，无需 sklearn 运行时。"""
    def __init__(self, model):
        self.n_features = model.n_features_in_
        self.n_outputs = model.n_outputs_
        n_trees = len(model.estimators_)
        self.n_trees = n_trees
        all_feat, all_thr, all_left, all_right, all_vals = [], [], [], [], []
        self.tree_offsets = [0]
        for est in model.estimators_:
            t = est.tree_
            n = t.node_count
            all_feat.append(t.nodes['feature'].astype(np.int32))
            all_thr.append(t.nodes['threshold'].astype(np.float64))
            all_left.append(t.nodes['left_child'].astype(np.int32))
            all_right.append(t.nodes['right_child'].astype(np.int32))
            all_vals.append(t.values.reshape(n, -1))
            self.tree_offsets.append(self.tree_offsets[-1] + n)
        self.feat = np.concatenate(all_feat)
        self.thr = np.concatenate(all_thr)
        self.left = np.concatenate(all_left)
        self.right = np.concatenate(all_right)
        self.vals = np.vstack(all_vals)
        self.root_nodes = np.array(self.tree_offsets[:-1], dtype=np.int32)
        # 修正子节点偏移为全局索引
        for i in range(len(self.tree_offsets) - 1):
            s = slice(self.tree_offsets[i], self.tree_offsets[i + 1])
            off = self.tree_offsets[i]
            l = self.left[s]
            self.left[s] = np.where(l >= 0, l + off, -1)
            r = self.right[s]
            self.right[s] = np.where(r >= 0, r + off, -1)

    def predict(self, X):
        X = np.atleast_2d(np.asarray(X, dtype=np.float64))
        results = np.zeros((X.shape[0], self.n_outputs))
        for i in range(X.shape[0]):
            x = X[i]
            nodes = self.root_nodes.copy()
            while True:
                active = self.left[nodes] != -1
                if not active.any():
                    break
                f_idx = self.feat[nodes]
                t_val = self.thr[nodes]
                x_vals = x[f_idx]
                go_left = x_vals <= t_val
                nodes = np.where(active, np.where(go_left, self.left[nodes], self.right[nodes]), nodes)
            results[i] = self.vals[nodes].mean(axis=0)
        return results


class CoordinatePredictor:
    """坐标预测器。优先用 sklearn 原生加载，失败时用 numpy 向量化实现。"""
    def __init__(self, model_a_path, model_b_path):
        self.rf_a = None
        self.rf_b = None
        self.simulation = True
        self.mode = "simulation"
        try:
            import joblib
            raw_a = joblib.load(model_a_path)
            raw_b = joblib.load(model_b_path)
            # 尝试 sklearn 原生 predict
            _ = raw_a.predict(np.array([[0, 0, 1, 1]]))
            _ = raw_b.predict(np.array([[0.0, 0.0]]))
            self.rf_a = raw_a
            self.rf_b = raw_b
            self.simulation = False
            self.mode = "sklearn"
            print(f"信息 | 坐标 | sklearn 原生模型加载成功 "
                  f"A={model_a_path} B={model_b_path}", flush=True)
        except Exception as e:
            print(f"警告 | 坐标 | sklearn 原生加载失败({e})，尝试 numpy 向量化", flush=True)
            try:
                import joblib
                import types
                # sklearn stub 加载（兼容旧版本 pickle）
                class Stub:
                    def __setstate__(self, s):
                        if isinstance(s, dict): self.__dict__.update(s)
                        elif isinstance(s, tuple):
                            for i in s:
                                if isinstance(i, dict): self.__dict__.update(i)
                    def __new__(cls, *a, **k): return object.__new__(cls)
                for n in ['sklearn', 'sklearn.ensemble', 'sklearn.ensemble._forest',
                          'sklearn.tree', 'sklearn.tree._classes', 'sklearn.tree._tree',
                          'sklearn.utils', 'sklearn.utils._joblib', 'sklearn.base',
                          'sklearn.exceptions', 'sklearn._config',
                          'scipy', 'scipy.sparse']:
                    if n not in sys.modules:
                        m = types.ModuleType(n)
                        m.__path__ = []
                        sys.modules[n] = m
                sys.modules['sklearn.ensemble._forest'].RandomForestRegressor = type('RFR', (Stub,), {})
                sys.modules['sklearn.tree._classes'].DecisionTreeRegressor = type('DTR', (Stub,), {})
                sys.modules['sklearn.tree._tree'].Tree = type('Tree', (Stub,), {})
                sys.modules['scipy.sparse'].csr_matrix = type('csr', (Stub,), {})
                raw_a = joblib.load(model_a_path)
                raw_b = joblib.load(model_b_path)
                self.rf_a = NumpyRandomForest(raw_a)
                self.rf_b = NumpyRandomForest(raw_b)
                self.simulation = False
                self.mode = "numpy"
                print(f"信息 | 坐标 | numpy 向量化模型加载成功 "
                      f"A={model_a_path}({self.rf_a.n_trees}树) "
                      f"B={model_b_path}({self.rf_b.n_trees}树)", flush=True)
            except Exception as e2:
                print(f"警告 | 坐标 | numpy 加载也失败({e2})，降级为接口模拟", flush=True)

    def predict(self, stream_id, x1, y1, x2, y2, img_w, img_h):
        """返回 (lon, lat, valid)。"""
        try:
            if not self.simulation and self.rf_a and self.rf_b:
                if stream_id == "A":
                    feat = np.array([[x1, y1, x2, y2]])
                    pred = self.rf_a.predict(feat)
                else:
                    cx = (x1 + x2) / 2.0
                    cy = (y1 + y2) / 2.0
                    feat = np.array([[cx, cy]])
                    pred = self.rf_b.predict(feat)
                lon, lat = float(pred[0][0]), float(pred[0][1])
                if math.isnan(lon) or math.isnan(lat) or math.isinf(lon) or math.isinf(lat):
                    return 0.0, 0.0, False
                if not (-180 <= lon <= 180) or not (-90 <= lat <= 90):
                    return 0.0, 0.0, False
                return lon, lat, True
            else:
                cx = (x1 + x2) / 2.0 / max(img_w, 1)
                cy = (y1 + y2) / 2.0 / max(img_h, 1)
                base_lon, base_lat = 121.035, 30.560
                lon = base_lon + (cx - 0.5) * 0.02
                lat = base_lat - cy * 0.01
                return lon, lat, True
        except Exception:
            return 0.0, 0.0, False


class MqttAisSubscriber:
    """MQTT AIS 订阅器，线程安全连接与重连。"""
    def __init__(self, store, host, port, client_id, username, password, topics):
        self.store = store
        self.host = host
        self.port = port
        self.client_id = client_id
        self.username = username
        self.password = password
        self.topics = topics
        self._stop = False
        self.connected = False
        self.client = None

    def _on_connect(self, client, userdata, flags, rc, properties=None):
        if rc == 0:
            self.connected = True
            self.client = client
            print(f"信息 | MQTT | 连接成功 {self.host}:{self.port}", flush=True)
            for t in self.topics:
                client.subscribe(t)
                print(f"信息 | MQTT | 订阅 {t}", flush=True)
        else:
            print(f"错误 | MQTT | 连接失败 rc={rc}", flush=True)

    def _on_message(self, client, userdata, msg):
        try:
            self.store.msg_count += 1
            raw = msg.payload.decode('utf-8', errors='replace').strip()
            print(f"信息 | MQTT | 收到消息 topic={msg.topic} len={len(raw)}", flush=True)
            ts_match = re.search(r'\*(\d{10})$', raw)
            timestamp = int(ts_match.group(1)) if ts_match else None
            if ts_match:
                raw = raw.replace(f'*{ts_match.group(1)}', '').strip()
            parsed = parse_ais_payload(raw)
            if parsed and "lon" in parsed:
                self.store.update(parsed["mmsi"], parsed["lon"], parsed["lat"],
                                  parsed.get("speed", 0), parsed.get("course", 0),
                                  timestamp)
                self.store.parse_ok += 1
                print(f"信息 | AIS | 解析成功 mmsi={parsed['mmsi']} "
                      f"lon={parsed['lon']:.6f} lat={parsed['lat']:.6f} "
                      f"type={parsed['msg_type']} 缓存={self.store.count()}",
                      flush=True)
            else:
                self.store.parse_fail += 1
                print(f"信息 | AIS | 解析失败 raw={raw[:40]}...", flush=True)
        except Exception as e:
            self.store.parse_fail += 1
            print(f"警告 | AIS | 消息处理异常: {e}", flush=True)

    def run(self):
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            print("警告 | MQTT | paho-mqtt 不可用，AIS 使用 replay 模式", flush=True)
            return
        while not self._stop:
            try:
                client = mqtt.Client(
                    callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
                    client_id=self.client_id)
                client.username_pw_set(self.username, self.password)
                client.on_connect = self._on_connect
                client.on_message = self._on_message
                client.connect(self.host, self.port, 60)
                client.loop_forever(retry_first_connection=True)
            except Exception as e:
                print(f"警告 | MQTT | 连接异常: {e}，5秒后重试", flush=True)
                time.sleep(5)
            if self._stop:
                break

    def stop(self):
        self._stop = True
        if self.client:
            try:
                self.client.disconnect()
            except Exception:
                pass


def match_detections_to_ais(detections, ais_snapshot, max_distance_km=0.5):
    """最近邻全局排序匹配。一个视觉框最多匹配一个 AIS，一个 AIS 最多匹配一个视觉框。"""
    candidates = []
    for det in detections:
        lon = det.get("longitude", 0)
        lat = det.get("latitude", 0)
        if not det.get("coordinate_valid", False):
            continue
        for mmsi, ais in ais_snapshot.items():
            d = haversine_km(lon, lat, ais["lon"], ais["lat"])
            if d <= max_distance_km:
                candidates.append((d, det["detection_id"], mmsi, ais))
    candidates.sort(key=lambda x: x[0])
    matched_dets = set()
    matched_mmsi = set()
    results = {}
    for dist, det_id, mmsi, ais in candidates:
        if det_id in matched_dets or mmsi in matched_mmsi:
            continue
        matched_dets.add(det_id)
        matched_mmsi.add(mmsi)
        results[det_id] = {
            "mmsi": mmsi,
            "ship_name": ais.get("ship_name", ""),
            "speed": ais.get("speed", 0),
            "course": ais.get("course", 0),
            "ais_distance_km": round(dist, 4),
            "ais_age_seconds": int(time.time() - ais.get("recv_ts", time.time())),
            "ais_matched": True,
        }
    return results


def handle_request(req, predictor, ais_store):
    """处理一帧批量请求。"""
    stream_id = req.get("stream_id", "")
    frame_seq = req.get("frame_sequence", 0)
    img_w = req.get("image_width", 2560)
    img_h = req.get("image_height", 1440)
    detections = req.get("detections", [])

    t0 = time.time()
    results = []
    for det in detections:
        det_id = det.get("detection_id", 0)
        x1, y1 = det.get("x1", 0), det.get("y1", 0)
        x2, y2 = det.get("x2", 0), det.get("y2", 0)
        lon, lat, valid = predictor.predict(stream_id, x1, y1, x2, y2, img_w, img_h)
        results.append({
            "detection_id": det_id,
            "longitude": round(lon, 6) if valid else 0.0,
            "latitude": round(lat, 6) if valid else 0.0,
            "coordinate_valid": valid,
            "ais_matched": False,
            "mmsi": "",
            "ship_name": "",
            "speed": 0.0,
            "course": 0.0,
            "ais_distance_km": 0.0,
            "ais_age_seconds": 0,
        })

    # AIS 匹配
    if any(r["coordinate_valid"] for r in results):
        snap = ais_store.snapshot()
        if snap:
            match_input = [{"detection_id": r["detection_id"],
                             "longitude": r["longitude"],
                             "latitude": r["latitude"],
                             "coordinate_valid": r["coordinate_valid"]}
                            for r in results]
            matches = match_detections_to_ais(match_input, snap, max_distance_km=0.5)
            for r in results:
                if r["detection_id"] in matches:
                    r.update(matches[r["detection_id"]])

    processing_ms = round((time.time() - t0) * 1000, 2)
    return {
        "stream_id": stream_id,
        "frame_sequence": frame_seq,
        "processing_ms": processing_ms,
        "results": results,
    }


def main():
    model_a = os.environ.get("COORD_MODEL_A", "weights/0121_random_forest_model.pkl")
    model_b = os.environ.get("COORD_MODEL_B", "weights/beishang_x-l.pkl")

    predictor = CoordinatePredictor(model_a, model_b)
    ais_store = AisStore(max_capacity=500, timeout_sec=30)

    # MQTT 配置
    mqtt_host = os.environ.get("AIS_MQTT_HOST", "")
    mqtt_port = int(os.environ.get("AIS_MQTT_PORT", "1883"))
    mqtt_client_id = os.environ.get("AIS_MQTT_CLIENT_ID", "")
    mqtt_user = os.environ.get("AIS_MQTT_USERNAME", "")
    mqtt_pass = os.environ.get("AIS_MQTT_PASSWORD", "")
    topics_str = os.environ.get("AIS_MQTT_TOPICS", "upAIS/base_2250,upAIS/base_2251")
    topics = [t.strip() for t in topics_str.split(",") if t.strip()]

    if not mqtt_host:
        print("警告 | MQTT | AIS_MQTT_HOST 未配置，AIS 使用 replay 模式（无实时数据）",
              flush=True)
    mqtt_sub = MqttAisSubscriber(ais_store, mqtt_host, mqtt_port,
                                 mqtt_client_id, mqtt_user, mqtt_pass, topics)
    mqtt_thread = threading.Thread(target=mqtt_sub.run, daemon=True)
    mqtt_thread.start()

    # AIS 清理线程
    def cleanup_loop():
        while True:
            time.sleep(10)
            n = ais_store.cleanup()
            if n > 0:
                print(f"信息 | AIS | 清理过期记录 {n} 条，剩余 {ais_store.count()} 条",
                      flush=True)
    threading.Thread(target=cleanup_loop, daemon=True).start()

    # 定期输出 AIS 统计
    def stats_loop():
        while True:
            time.sleep(30)
            print(f"信息 | AIS | 统计 msg={ais_store.msg_count} "
                  f"ok={ais_store.parse_ok} fail={ais_store.parse_fail} "
                  f"缓存={ais_store.count()} MQTT={mqtt_sub.connected}",
                  flush=True)
    threading.Thread(target=stats_loop, daemon=True).start()

    # Unix Domain Socket（多线程，每个连接一个线程）
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET_PATH)
    server.listen(8)
    server.settimeout(1.0)
    print(f"信息 | Sidecar | 监听 {SOCKET_PATH} 模式={predictor.mode} "
          f"AIS缓存={ais_store.count()}", flush=True)

    def handle_conn(conn):
        conn.settimeout(None)
        buf = b""
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    try:
                        req = json.loads(line)
                        resp = handle_request(req, predictor, ais_store)
                        conn.sendall(
                            (json.dumps(resp, separators=(',', ':')) + "\n")
                            .encode("utf-8"))
                    except json.JSONDecodeError:
                        conn.sendall(b'{"error":"invalid json"}\n')
                    except Exception as e:
                        conn.sendall(
                            json.dumps({"error": str(e)}).encode() + b"\n")
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except Exception:
                pass

    while True:
        try:
            conn, _ = server.accept()
            threading.Thread(target=handle_conn, args=(conn,),
                             daemon=True).start()
        except socket.timeout:
            continue
        except KeyboardInterrupt:
            break

    mqtt_sub.stop()
    server.close()
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)


if __name__ == "__main__":
    main()
