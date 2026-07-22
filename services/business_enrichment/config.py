# -*- coding: utf-8 -*-
"""业务增强服务配置管理。

从环境变量和 YAML 配置文件加载配置，支持 schema 校验和生产模式门禁。
"""
import os
import sys

DEFAULTS = {
    "environment": "development",
    "log_level": "INFO",
    "socket_path": "/tmp/hangzhouwan-business.sock",
    "coordinate_mode": "sklearn",
    "model_a_path": "weights/0121_random_forest_model.pkl",
    "model_b_path": "weights/beishang_x-l.pkl",
    "reference_width": 2560,
    "reference_height": 1440,
    "ais_max_capacity": 500,
    "ais_data_timeout_sec": 30,
    "ais_max_extrapolation_sec": 120,
    "ais_cleanup_interval_sec": 10,
    "stats_interval_sec": 30,
    "mqtt_host": "",
    "mqtt_port": 1883,
    "mqtt_client_id": "",
    "mqtt_username": "",
    "mqtt_password": "",
    "mqtt_topics": "upAIS/base_2250,upAIS/base_2251",
    "mqtt_keepalive": 60,
    "mqtt_reconnect_sec": 5,
    "stream_a_ais_max_distance_m": 500,
    "stream_b_ais_max_distance_m": 500,
    "request_timeout_ms": 30,
    "max_connections": 8,
    "log_dir": "logs/business",
    "jsonl_dir": "artifacts/internal-development",
    "disk_threshold_mb": 500,
    "enable_ais_capture": False,
    "enable_evidence_recording": False,
}

VALID_ENVS = ("development", "staging", "production")
VALID_COORD_MODES = ("sklearn", "numpy", "mock", "off")


class ConfigError(Exception):
    pass


class BusinessConfig:
    """配置对象，从环境变量加载，支持 schema 校验。"""

    def __init__(self, **overrides):
        self._data = dict(DEFAULTS)
        self._data.update(overrides)
        self._load_from_env()
        self._validate()

    def _load_from_env(self):
        env_map = {
            "environment": "HZW_ENVIRONMENT",
            "socket_path": "HANGZHOUWAN_BUSINESS_SOCK",
            "coordinate_mode": "COORD_MODE",
            "model_a_path": "COORD_MODEL_A",
            "model_b_path": "COORD_MODEL_B",
            "mqtt_host": "AIS_MQTT_HOST",
            "mqtt_client_id": "AIS_MQTT_CLIENT_ID",
            "mqtt_username": "AIS_MQTT_USERNAME",
            "mqtt_password": "AIS_MQTT_PASSWORD",
            "mqtt_topics": "AIS_MQTT_TOPICS",
            "log_dir": "HZW_LOG_DIR",
            "jsonl_dir": "HZW_JSONL_DIR",
        }
        int_map = {
            "mqtt_port": "AIS_MQTT_PORT",
            "mqtt_keepalive": "AIS_MQTT_KEEPALIVE",
            "mqtt_reconnect_sec": "AIS_MQTT_RECONNECT_SEC",
            "reference_width": "HZW_REF_WIDTH",
            "reference_height": "HZW_REF_HEIGHT",
            "ais_max_capacity": "AIS_MAX_CAPACITY",
            "ais_data_timeout_sec": "AIS_DATA_TIMEOUT_SEC",
            "ais_max_extrapolation_sec": "AIS_MAX_EXTRAPOLATION_SEC",
            "stream_a_ais_max_distance_m": "STREAM_A_AIS_MAX_DISTANCE_M",
            "stream_b_ais_max_distance_m": "STREAM_B_AIS_MAX_DISTANCE_M",
            "request_timeout_ms": "BUSINESS_TIMEOUT_MS",
            "disk_threshold_mb": "DISK_THRESHOLD_MB",
        }
        for key, env_key in env_map.items():
            val = os.environ.get(env_key)
            if val is not None:
                self._data[key] = val
        for key, env_key in int_map.items():
            val = os.environ.get(env_key)
            if val is not None:
                try:
                    self._data[key] = int(val)
                except ValueError:
                    raise ConfigError(f"env {env_key}={val} must be integer")
        if self._data.get("mqtt_topics"):
            self._data["mqtt_topics"] = [
                t.strip() for t in str(self._data["mqtt_topics"]).split(",") if t.strip()
            ]
        else:
            self._data["mqtt_topics"] = []

    def _validate(self):
        env = self._data["environment"]
        if env not in VALID_ENVS:
            raise ConfigError(f"environment must be one of {VALID_ENVS}, got '{env}'")
        mode = self._data["coordinate_mode"]
        if mode not in VALID_COORD_MODES:
            raise ConfigError(
                f"coordinate_mode must be one of {VALID_COORD_MODES}, got '{mode}'"
            )
        if env == "production" and mode == "mock":
            raise ConfigError(
                "production environment forbids mock coordinate_mode"
            )
        if env == "production" and mode == "off":
            raise ConfigError(
                "production environment forbids off coordinate_mode"
            )
        if mode in ("sklearn", "numpy"):
            for key in ("model_a_path", "model_b_path"):
                p = self._data[key]
                if not p or not os.path.exists(p):
                    raise ConfigError(
                        f"coordinate_mode={mode} requires existing model file: {key}={p}"
                    )

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        try:
            return self._data[name]
        except KeyError:
            raise AttributeError(name)

    def get(self, key, default=None):
        return self._data.get(key, default)

    def to_dict(self):
        d = dict(self._data)
        if d.get("mqtt_password"):
            d["mqtt_password"] = "***"
        return d
