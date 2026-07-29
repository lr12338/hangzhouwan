# -*- coding: utf-8 -*-
"""业务增强服务主入口。

用法：
  python3 -m services.business_enrichment.app                    # 正常启动
  python3 -m services.business_enrichment.app --mqtt-probe --seconds 300  # MQTT探测
  python3 -m services.business_enrichment.app --health-only      # 仅输出健康状态
"""
import argparse
import json
import os
import socket
import sys
import threading
import time

# 确保可以 import services.business_enrichment
if __name__ == "__main__" and __package__ is None:
    _repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    sys.path.insert(0, _repo_root)

from services.business_enrichment import __version__, PROTOCOL_VERSION
from services.business_enrichment.config import BusinessConfig, ConfigError
from services.business_enrichment.coordinate import create_predictor
from services.business_enrichment.ais.store import AisStore
from services.business_enrichment.ais.subscriber import MqttAisSubscriber
from services.business_enrichment.ais.replay import AisReplayer
from services.business_enrichment.matching.visual_ais_matcher import VisualAisMatcher
from services.business_enrichment.health.status import HealthStatus, HEALTHY, DEGRADED, FAILED
from services.business_enrichment import protocol as proto
from services.business_enrichment.log_rotation import DiskProtector, LogRotator


class BusinessEnrichmentService:
    """业务增强服务主类。"""

    def __init__(self, config: BusinessConfig):
        self.config = config
        self.predictor = None
        self.ais_store = AisStore(
            max_capacity=config.ais_max_capacity,
            timeout_sec=config.ais_data_timeout_sec,
        )
        self.mqtt_sub = None
        self.matcher = VisualAisMatcher(
            stream_a_max_distance_m=config.stream_a_ais_max_distance_m,
            stream_b_max_distance_m=config.stream_b_ais_max_distance_m,
            data_timeout_sec=config.ais_data_timeout_sec,
            max_extrapolation_sec=config.ais_max_extrapolation_sec,
        )
        self.health = HealthStatus(
            service_version=__version__,
            protocol_version=PROTOCOL_VERSION,
            coordinate_mode=config.coordinate_mode,
        )
        self._stop = False
        self._server = None
        self._conn_count = 0
        self._evidence_dir = None
        self._evidence_counts = {"A": 0, "B": 0}
        self.disk_protector = DiskProtector(
            threshold_mb=config.disk_threshold_mb,
        )
        self.disk_protector.register_callback(self._on_disk_protect)

    def start_predictor(self):
        """加载坐标预测器。"""
        if self.config.coordinate_mode == "off":
            print("信息 | 坐标 | 模式=off，坐标预测禁用", flush=True)
            return
        self.predictor = create_predictor(
            self.config.coordinate_mode,
            self.config.model_a_path,
            self.config.model_b_path,
            self.config.reference_width,
            self.config.reference_height,
        )
        info = self.predictor.model_info()
        self.health.model_a_loaded = info.get("loaded", False)
        self.health.model_b_loaded = info.get("loaded", False)
        print(f"信息 | 坐标 | 模式={self.config.coordinate_mode} 加载完成 "
              f"{info}", flush=True)

    def start_mqtt(self):
        """启动 MQTT 订阅。"""
        if not self.config.mqtt_host:
            print("信息 | MQTT | 未配置 host，AIS 使用 replay 模式", flush=True)
            return
        self.mqtt_sub = MqttAisSubscriber(
            self.ais_store,
            self.config.mqtt_host,
            self.config.mqtt_port,
            self.config.mqtt_client_id,
            self.config.mqtt_username,
            self.config.mqtt_password,
            self.config.mqtt_topics,
            self.config.mqtt_keepalive,
            self.config.mqtt_reconnect_sec,
            event_topic=self.config.mqtt_event_topic,
            offline_max_records=self.config.mqtt_offline_max_records,
            offline_max_bytes=self.config.mqtt_offline_max_bytes,
        )
        threading.Thread(target=self.mqtt_sub.run, daemon=True).start()

    def start_cleanup_thread(self):
        def _loop():
            while not self._stop:
                time.sleep(self.config.ais_cleanup_interval_sec)
                n = self.ais_store.cleanup()
                if n > 0:
                    print(f"信息 | AIS | 清理过期 {n} 条，剩余 {self.ais_store.count()}",
                          flush=True)
        threading.Thread(target=_loop, daemon=True).start()

    def start_stats_thread(self):
        def _loop():
            while not self._stop:
                time.sleep(self.config.stats_interval_sec)
                s = self.ais_store.stats()
                mqtt_s = self.mqtt_sub.stats() if self.mqtt_sub else {}
                print(f"信息 | AIS | msg={s['msg_count']} ok={s['parse_ok']} "
                      f"fail={s['parse_fail']} 缓存={s['cache_count']} "
                      f"MQTT={mqtt_s.get('connected', False)}", flush=True)
        threading.Thread(target=_loop, daemon=True).start()

    def handle_request(self, req):
        """处理一帧批量请求。"""
        t0 = time.time()
        stream_id = req.get("stream_id", "")
        frame_seq = req.get("frame_sequence", 0)
        img_w = req.get("image_width", self.config.reference_width)
        img_h = req.get("image_height", self.config.reference_height)
        detections = req.get("detections", [])
        req_id = req.get("request_id", "")
        deadline_ms = req.get("deadline_ms", self.config.request_timeout_ms)
        frame_wall_time = req.get("frame_wall_time", 0)
        if frame_wall_time == 0:
            frame_wall_time = None

        self.health.request_count += 1
        results = []
        coord_mode = self.config.coordinate_mode

        # 坐标预测
        if self.predictor is not None:
            det_input = []
            for det in detections:
                det_input.append({
                    "x1": det.get("x1", 0),
                    "y1": det.get("y1", 0),
                    "x2": det.get("x2", 0),
                    "y2": det.get("y2", 0),
                    "image_width": img_w,
                    "image_height": img_h,
                })
            try:
                coord_results = self.predictor.predict(stream_id, det_input)
            except Exception as e:
                self.health.error_count += 1
                coord_results = [(0.0, 0.0, False)] * len(det_input)
            for i, det in enumerate(detections):
                lon, lat, valid = coord_results[i] if i < len(coord_results) else (0.0, 0.0, False)
                results.append({
                    "detection_id": det.get("detection_id", i),
                    "score": det.get("score", 0),
                    "longitude": lon,
                    "latitude": lat,
                    "coordinate_valid": valid,
                    "ais_matched": False,
                    "mmsi": "",
                    "ship_name": "",
                    "speed": 0.0,
                    "course": 0.0,
                    "ais_distance_km": 0.0,
                    "ais_age_ms": 0,
                    "extrapolated": False,
                    "match_score": 0.0,
                    "reject_reason": "",
                })
        else:
            for i, det in enumerate(detections):
                results.append({
                    "detection_id": det.get("detection_id", i),
                    "score": det.get("score", 0),
                    "longitude": 0.0,
                    "latitude": 0.0,
                    "coordinate_valid": False,
                    "ais_matched": False,
                    "mmsi": "",
                    "ship_name": "",
                    "speed": 0.0,
                    "course": 0.0,
                    "ais_distance_km": 0.0,
                    "ais_age_ms": 0,
                    "extrapolated": False,
                    "match_score": 0.0,
                    "reject_reason": "",
                })

        # AIS 匹配
        if any(r["coordinate_valid"] for r in results):
            snap = self.ais_store.snapshot()
            if snap:
                match_input = [{"detection_id": r["detection_id"],
                                 "longitude": r["longitude"],
                                 "latitude": r["latitude"],
                                 "coordinate_valid": r["coordinate_valid"]}
                                for r in results]
                matches = self.matcher.match(stream_id, match_input, snap,
                                             frame_wall_time, coord_mode)
                for r in results:
                    if r["detection_id"] in matches:
                        m = matches[r["detection_id"]]
                        r.update({
                            "ais_matched": True,
                            "mmsi": m["mmsi"],
                            "ship_name": m.get("ship_name", ""),
                            "speed": m.get("speed", 0),
                            "course": m.get("course", 0),
                            "ais_distance_km": round(m["distance_m"] / 1000.0, 6),
                            "ais_age_ms": m["ais_age_ms"],
                            "extrapolated": m.get("extrapolated", False),
                            "match_score": m.get("match_score", 0),
                            "ais_lon": m.get("ais_lon", 0),
                            "ais_lat": m.get("ais_lat", 0),
                            "ais_lon_aligned": m.get("ais_lon_aligned", 0),
                            "ais_lat_aligned": m.get("ais_lat_aligned", 0),
                        })

        # 证据录制
        if self.config.enable_evidence_recording and any(r.get("ais_matched") for r in results):
            self._record_evidence(stream_id, frame_seq, results, req)

        processing_ms = round((time.time() - t0) * 1000, 2)
        if results and self.mqtt_sub:
            self.mqtt_sub.publish_event({
                "schema_version": 1,
                "timestamp_ms": int(time.time() * 1000),
                "stream_id": stream_id,
                "frame_sequence": frame_seq,
                "coordinate_mode": coord_mode,
                "results": results,
            })
        return proto.make_response(
            stream_id, frame_seq, results, processing_ms,
            coordinate_mode=coord_mode,
            response_status="OK",
            request_id=req_id,
        )

    def _on_disk_protect(self, protected):
        """磁盘保护回调。"""
        if protected:
            # 停止抓拍和录制，保留核心推流
            if self.config.enable_evidence_recording:
                self.config._data["enable_evidence_recording"] = False
            if self.mqtt_sub:
                self.mqtt_sub.enable_capture = False
        # 恢复时不自动恢复录制（需要人工确认）

    def _init_evidence(self):
        if not self.config.enable_evidence_recording:
            return
        self._evidence_dir = os.path.join(
            self.config.jsonl_dir, "ais_validation"
        )
        for sid in ("A", "B"):
            os.makedirs(os.path.join(self._evidence_dir, sid), exist_ok=True)

    def _record_evidence(self, stream_id, frame_seq, results, req):
        if not self._evidence_dir:
            return
        ts = time.time()
        for r in results:
            if not r.get("ais_matched"):
                continue
            # 检测框
            det_box = None
            for d in detections:
                if d.get("detection_id") == r["detection_id"]:
                    det_box = {"x1": d.get("x1", 0), "y1": d.get("y1", 0),
                               "x2": d.get("x2", 0), "y2": d.get("y2", 0),
                               "score": d.get("score", 0)}
                    break
            rec = {
                "time": ts,
                "stream_id": stream_id,
                "frame_sequence": frame_seq,
                "detection_id": r["detection_id"],
                "detection_box": det_box,
                # 视觉预测坐标
                "visual_longitude": r["longitude"],
                "visual_latitude": r["latitude"],
                # AIS 原始坐标
                "ais_lon": r.get("ais_lon", 0),
                "ais_lat": r.get("ais_lat", 0),
                # AIS 时间对齐后坐标
                "ais_lon_aligned": r.get("ais_lon_aligned", 0),
                "ais_lat_aligned": r.get("ais_lat_aligned", 0),
                "mmsi": r["mmsi"],
                "ship_name": r.get("ship_name", ""),
                "match_distance_m": r.get("ais_distance_km", 0) * 1000,
                "ais_age_ms": r.get("ais_age_ms", 0),
                "extrapolated": r.get("extrapolated", False),
                "match_score": r.get("match_score", 0),
                "reject_reason": r.get("reject_reason", ""),
            }
            path = os.path.join(self._evidence_dir, stream_id,
                                f"evidence_{int(ts)}.jsonl")
            with open(path, "a", encoding="utf-8") as f:
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")
            self._evidence_counts[stream_id] = self._evidence_counts.get(stream_id, 0) + 1

    def handle_health(self):
        self.health.ais_cache_count = self.ais_store.count()
        if self.mqtt_sub:
            s = self.mqtt_sub.stats()
            self.health.mqtt_connected = s["connected"]
            self.health.mqtt_last_message_time = s["last_message_time"]
        return self.health.to_dict()

    def run_server(self):
        """启动 Unix Domain Socket 服务器。"""
        sock_path = self.config.socket_path
        if os.path.exists(sock_path):
            os.unlink(sock_path)
        self._server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._server.bind(sock_path)
        # Video 与 Business 使用独立账户、共享 hangzhouwan 组。Unix Socket
        # 连接需要写权限；显式设为 0660，避免 UMask=0027 生成 0750 后组成员
        # 只能读取却无法 connect。
        os.chmod(sock_path, 0o660)
        self._server.listen(self.config.max_connections)
        self._server.settimeout(1.0)
        print(f"信息 | Sidecar | 监听 {sock_path} 模式={self.config.coordinate_mode} "
              f"缓存={self.ais_store.count()}", flush=True)

        while not self._stop:
            try:
                conn, _ = self._server.accept()
                self._conn_count += 1
                self.health.uds_connections = self._conn_count
                threading.Thread(
                    target=self._handle_conn, args=(conn,), daemon=True
                ).start()
            except socket.timeout:
                continue
            except OSError:
                break

        if self.mqtt_sub:
            self.mqtt_sub.stop()
        self._server.close()
        if os.path.exists(sock_path):
            os.unlink(sock_path)

    def _handle_conn(self, conn):
        conn.settimeout(None)
        buf = b""
        try:
            while not self._stop:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                buf += chunk
                # 尝试长度前缀协议（v2）
                try:
                    while len(buf) >= proto.HEADER_SIZE:
                        obj, buf = proto.decode_stream(buf)
                        if obj is None:
                            break
                        resp = self._process_message(obj)
                        conn.sendall(proto.encode_message(resp))
                except proto.ProtocolError:
                    # 回退到旧版换行分隔协议（v1兼容）
                    while True:
                        obj, buf = proto.decode_line_json(buf)
                        if obj is None:
                            break
                        if obj == "INVALID":
                            conn.sendall(proto.encode_line_json(
                                {"error": "invalid json"}
                            ))
                            continue
                        resp = self._process_message(obj)
                        conn.sendall(proto.encode_line_json(resp))
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except Exception:
                pass

    def _process_message(self, msg):
        """处理单条消息，支持 enrich 和 health 请求。"""
        if not isinstance(msg, dict):
            return {"error": "invalid request"}
        action = msg.get("action", "enrich")
        if action == "health":
            return self.handle_health()
        return self.handle_request(msg)

    def stop(self):
        self._stop = True

    def mqtt_probe(self, seconds):
        """MQTT 探测模式：连接、订阅、统计消息。"""
        print(f"信息 | MQTT探测 | 启动 {seconds}秒探测", flush=True)
        if not self.config.mqtt_host:
            print("错误 | MQTT探测 | 未配置 AIS_MQTT_HOST", flush=True)
            return {"connected": False, "error": "no host configured"}
        self.start_mqtt()
        start = time.time()
        while time.time() - start < seconds and not self._stop:
            time.sleep(1)
        s = self.ais_store.stats()
        mqtt_s = self.mqtt_sub.stats() if self.mqtt_sub else {}
        now = time.time()
        last_msg_age = (now - mqtt_s.get("last_message_time", 0)) if mqtt_s.get("last_message_time") else -1
        result = {
            "mqtt_connected": mqtt_s.get("connected", False),
            "topics": self.config.mqtt_topics,
            "total_messages": s["msg_count"],
            "topic_counts": mqtt_s.get("topic_counts", {}),
            "ais_decode_ok": s["parse_ok"],
            "ais_decode_fail": s["parse_fail"],
            "msg_type_counts": s["msg_type_counts"],
            "valid_position_messages": s["parse_ok"],
            "ais_cache_ships": s["cache_count"],
            "last_message_age_sec": round(last_msg_age, 1) if last_msg_age >= 0 else -1,
            "mqtt_reconnect_count": mqtt_s.get("reconnect_count", 0),
        }
        if s["msg_count"] == 0:
            result["verdict"] = "MQTT协议连接订阅通过，但无真实AIS数据（区域内可能无船）"
        else:
            result["verdict"] = "MQTT真实AIS数据验证通过" if s["parse_ok"] > 0 else "MQTT连接通过但AIS解析全部失败"
        print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
        if self.mqtt_sub:
            self.mqtt_sub.stop()
        return result


def main():
    parser = argparse.ArgumentParser(description="杭州湾业务增强服务")
    parser.add_argument("--config", type=str, default="",
                        help="application.yaml 配置文件路径")
    parser.add_argument("--mqtt-probe", action="store_true", help="MQTT探测模式")
    parser.add_argument("--seconds", type=int, default=300, help="探测时长（秒）")
    parser.add_argument("--health-only", action="store_true", help="仅输出健康状态")
    parser.add_argument("--replay", type=str, default="", help="AIS replay 文件路径")
    args = parser.parse_args()

    try:
        config = BusinessConfig(config_path=args.config if args.config else None)
    except ConfigError as e:
        print(f"致命 | 配置 | {e}", flush=True)
        sys.exit(1)

    service = BusinessEnrichmentService(config)

    if args.mqtt_probe:
        result = service.mqtt_probe(args.seconds)
        sys.exit(0 if result.get("mqtt_connected") else 1)

    if args.health_only:
        print(json.dumps(service.handle_health(), ensure_ascii=False, indent=2))
        sys.exit(0)

    # 正常启动
    service._init_evidence()
    service.start_predictor()
    service.start_mqtt()
    if args.replay:
        replayer = AisReplayer(service.ais_store, replay_speed=10, loop=True)
        replayer.replay_in_background(args.replay)
        print(f"信息 | AIS | replay 后台回放 {args.replay}", flush=True)
    service.start_cleanup_thread()
    service.start_stats_thread()
    service.disk_protector.start_monitor()

    try:
        service.run_server()
    except KeyboardInterrupt:
        pass
    finally:
        service.stop()


if __name__ == "__main__":
    main()
