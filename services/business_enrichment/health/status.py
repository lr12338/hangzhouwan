# -*- coding: utf-8 -*-
"""健康检查模块。"""
import time

HEALTHY = "HEALTHY"
DEGRADED = "DEGRADED"
FAILED = "FAILED"


class HealthStatus:
    """业务 Sidecar 健康状态。"""

    def __init__(self, service_version="1.0.0", protocol_version="2",
                 coordinate_mode="sklearn"):
        self.service_version = service_version
        self.protocol_version = protocol_version
        self.coordinate_mode = coordinate_mode
        self.model_a_loaded = False
        self.model_b_loaded = False
        self.mqtt_connected = False
        self.mqtt_last_message_time = 0.0
        self.ais_cache_count = 0
        self.uds_connections = 0
        self.request_count = 0
        self.timeout_count = 0
        self.error_count = 0
        self.start_time = time.time()

    def update(self, **kwargs):
        for k, v in kwargs.items():
            if hasattr(self, k):
                setattr(self, k, v)

    def status(self):
        """计算当前健康状态。"""
        if not self.model_a_loaded and not self.model_b_loaded:
            return FAILED
        if self.error_count > 100:
            return FAILED
        if self.coordinate_mode == "off":
            return DEGRADED
        return HEALTHY

    def to_dict(self):
        return {
            "service_version": self.service_version,
            "protocol_version": self.protocol_version,
            "coordinate_mode": self.coordinate_mode,
            "model_a_loaded": self.model_a_loaded,
            "model_b_loaded": self.model_b_loaded,
            "mqtt_connected": self.mqtt_connected,
            "mqtt_last_message_time": self.mqtt_last_message_time,
            "ais_cache_count": self.ais_cache_count,
            "uds_connections": self.uds_connections,
            "request_count": self.request_count,
            "timeout_count": self.timeout_count,
            "error_count": self.error_count,
            "uptime_seconds": round(time.time() - self.start_time, 1),
            "status": self.status(),
        }


class HealthChecker:
    """视频服务端健康检查（由 C++ 端通过 health 请求获取）。"""

    def __init__(self, output_fps_min=9, reconnects_per_hour=2):
        self.stream_status = {}
        self.output_fps_min = float(output_fps_min)
        self.reconnects_per_hour = int(reconnects_per_hour)

    def update_stream(self, stream_id, rtsp_connected, rtmp_connected,
                      output_fps, inference_fps, last_frame_time,
                      reconnect_count, tpu_status="ok",
                      business_status="HEALTHY", degraded_mode=""):
        self.stream_status[stream_id] = {
            "rtsp_connected": rtsp_connected,
            "rtmp_connected": rtmp_connected,
            "output_fps": output_fps,
            "inference_fps": inference_fps,
            "last_frame_time": last_frame_time,
            "reconnect_count": reconnect_count,
            "tpu_status": tpu_status,
            "business_status": business_status,
            "degraded_mode": degraded_mode,
        }

    def compute_stream_health(self, stream_id, output_fps, reconnect_count_hour):
        """计算单路健康状态。

        规则：
          output_fps >= 配置阈值（生产默认9）: HEALTHY
          output_fps 低于配置阈值: DEGRADED
          output_fps < 5 持续60秒: FAILED
          每小时重连超过配置阈值: 告警(DEGRADED)
        """
        if output_fps < 5:
            return FAILED
        if output_fps < self.output_fps_min:
            return DEGRADED
        if reconnect_count_hour > self.reconnects_per_hour:
            return DEGRADED
        return HEALTHY
