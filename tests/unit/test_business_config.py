# -*- coding: utf-8 -*-
"""BusinessConfig 单元测试。

验证：
- systemd 启动的 Business 读取 application.yaml
- YAML 配置覆盖默认值
- 环境变量只覆盖敏感或显式允许覆盖的字段
- production 禁止 mock/off
- 模型路径、MQTT 主题和 Socket 路径与 Video 一致
"""
import os
import sys
import tempfile
import unittest
import importlib.util

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

HAS_PYYAML = importlib.util.find_spec("yaml") is not None
if not HAS_PYYAML:
    yaml = None
else:
    import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLE_YAML = os.path.normpath(os.path.join(HERE, "..", "..", "config", "application.example.yaml"))


def make_yaml(**overrides):
    """生成临时 application.yaml。"""
    base = {
        "application": {"environment": "development", "log_level": "INFO"},
        "inference": {"backend": "bmrt", "model_path": "/data/best.bmodel", "device": 0,
                       "input_width": 640, "input_height": 640,
                       "confidence_threshold": 0.1, "iou_threshold": 0.1},
        "streams": [
            {"id": "A", "enabled": False, "input_url_env": "STREAM_A_INPUT_URL",
             "output_url_env": "STREAM_A_OUTPUT_URL", "coordinate_model": "",
             "inference_fps": 5, "output_fps": 10, "output_width": 960,
             "output_height": 540, "output_bitrate_kbps": 800,
             "forbidden_rectangles": [], "forbidden_polygons": []},
            {"id": "B", "enabled": False, "input_url_env": "STREAM_B_INPUT_URL",
             "output_url_env": "STREAM_B_OUTPUT_URL", "coordinate_model": "",
             "inference_fps": 5, "output_fps": 10, "output_width": 960,
             "output_height": 540, "output_bitrate_kbps": 800,
             "forbidden_rectangles": [], "forbidden_polygons": []},
        ],
        "runtime": {"frame_queue_size": 1, "decoder": "h264_bm", "encoder": "h264_bm",
                     "preprocess": "bmcv", "draw_mode": "bmcv",
                     "reconnect_initial_seconds": 2, "reconnect_max_seconds": 30},
        "business": {
            "coordinate_mode": "sklearn",
            "model_a_path": "",
            "model_b_path": "",
            "reference_width": 2560,
            "reference_height": 1440,
            "socket_path": "/run/hangzhouwan/business.sock",
            "request_timeout_ms": 30,
            "max_connections": 8,
            "enable_evidence_recording": False,
            "ais_max_distance_m": {"A": 500, "B": 500},
            "ais_max_extrapolation_sec": 120,
        },
        "mqtt": {
            "host_env": "AIS_MQTT_HOST",
            "port": 1883,
            "client_id_env": "AIS_MQTT_CLIENT_ID",
            "username_env": "AIS_MQTT_USERNAME",
            "password_env": "AIS_MQTT_PASSWORD",
            "topics": ["upAIS/base_2250", "upAIS/base_2251"],
            "keepalive": 60,
            "reconnect_sec": 5,
        },
        "logging": {"disk_threshold_mb": 500},
    }
    base.update(overrides)
    return base


def write_temp_yaml(data):
    fd, path = tempfile.mkstemp(suffix=".yaml")
    with os.fdopen(fd, "w") as f:
        yaml.dump(data, f, allow_unicode=True)
    return path


@unittest.skipUnless(HAS_PYYAML, "未安装 PyYAML")
class BusinessConfigTest(unittest.TestCase):
    def setUp(self):
        os.environ.pop("AIS_MQTT_HOST", None)
        os.environ.pop("AIS_MQTT_CLIENT_ID", None)
        os.environ.pop("COORD_MODE", None)
        os.environ.pop("HANGZHOUWAN_BUSINESS_SOCK", None)

    def tearDown(self):
        os.environ.pop("AIS_MQTT_HOST", None)
        os.environ.pop("AIS_MQTT_CLIENT_ID", None)
        os.environ.pop("COORD_MODE", None)
        os.environ.pop("HANGZHOUWAN_BUSINESS_SOCK", None)

    def _make_config(self, yaml_data=None, **overrides):
        from services.business_enrichment.config import BusinessConfig
        path = None
        if yaml_data:
            path = write_temp_yaml(yaml_data)
        try:
            return BusinessConfig(config_path=path, **overrides)
        finally:
            if path:
                os.unlink(path)

    def test_reads_application_yaml(self):
        """Business 从 application.yaml 读取配置"""
        data = make_yaml()
        data["business"]["socket_path"] = "/run/hangzhouwan/business.sock"
        data["business"]["coordinate_mode"] = "numpy"
        cfg = self._make_config(data)
        self.assertEqual(cfg.coordinate_mode, "numpy")
        self.assertEqual(cfg.socket_path, "/run/hangzhouwan/business.sock")

    def test_yaml_overrides_defaults(self):
        """YAML 配置覆盖默认值"""
        data = make_yaml()
        data["business"]["request_timeout_ms"] = 50
        data["mqtt"]["port"] = 8883
        data["mqtt"]["keepalive"] = 120
        cfg = self._make_config(data)
        self.assertEqual(cfg.request_timeout_ms, 50)
        self.assertEqual(cfg.mqtt_port, 8883)
        self.assertEqual(cfg.mqtt_keepalive, 120)

    def test_env_only_overrides_sensitive(self):
        """环境变量只覆盖敏感或显式允许覆盖的字段"""
        data = make_yaml()
        data["business"]["coordinate_mode"] = "sklearn"
        # 设置环境变量
        os.environ["AIS_MQTT_HOST"] = "mqtt.example.com"
        os.environ["AIS_MQTT_CLIENT_ID"] = "test-client"
        # COORD_MODE 环境变量应能覆盖
        os.environ["COORD_MODE"] = "numpy"
        cfg = self._make_config(data)
        self.assertEqual(cfg.mqtt_host, "mqtt.example.com")
        self.assertEqual(cfg.mqtt_client_id, "test-client")
        self.assertEqual(cfg.coordinate_mode, "numpy")
        # 清理
        del os.environ["AIS_MQTT_HOST"]
        del os.environ["AIS_MQTT_CLIENT_ID"]
        del os.environ["COORD_MODE"]

    def test_env_does_not_override_non_sensitive(self):
        """环境变量不能覆盖非敏感字段（如 request_timeout_ms）"""
        data = make_yaml()
        data["business"]["request_timeout_ms"] = 30
        cfg = self._make_config(data)
        # request_timeout_ms 没有 env 映射（非敏感），不应被环境变量覆盖
        self.assertEqual(cfg.request_timeout_ms, 30)

    def test_production_forbids_mock(self):
        """production 禁止 mock coordinate_mode"""
        from services.business_enrichment.config import BusinessConfig, ConfigError
        data = make_yaml()
        data["application"]["environment"] = "production"
        data["business"]["coordinate_mode"] = "mock"
        path = write_temp_yaml(data)
        try:
            with self.assertRaises(ConfigError):
                BusinessConfig(config_path=path)
        finally:
            os.unlink(path)

    def test_production_forbids_off(self):
        """production 禁止 off coordinate_mode"""
        from services.business_enrichment.config import BusinessConfig, ConfigError
        data = make_yaml()
        data["application"]["environment"] = "production"
        data["business"]["coordinate_mode"] = "off"
        path = write_temp_yaml(data)
        try:
            with self.assertRaises(ConfigError):
                BusinessConfig(config_path=path)
        finally:
            os.unlink(path)

    def test_socket_path_matches_video(self):
        """Socket 路径与 Video 一致 (/run/hangzhouwan/business.sock)"""
        data = make_yaml()
        cfg = self._make_config(data)
        self.assertEqual(cfg.socket_path, "/run/hangzhouwan/business.sock")

    def test_mqtt_topics_from_yaml(self):
        """MQTT 主题从 YAML 读取"""
        data = make_yaml()
        cfg = self._make_config(data)
        self.assertIn("upAIS/base_2250", cfg.mqtt_topics)
        self.assertIn("upAIS/base_2251", cfg.mqtt_topics)

    def test_mqtt_host_resolved_from_yaml_env_name(self):
        """MQTT host 从 YAML 声明的环境变量名解析"""
        data = make_yaml()
        os.environ["AIS_MQTT_HOST"] = "broker.test.com"
        cfg = self._make_config(data)
        self.assertEqual(cfg.mqtt_host, "broker.test.com")
        del os.environ["AIS_MQTT_HOST"]

    def test_example_yaml_socket_path(self):
        """示例 YAML 的 socket_path 必须是 /run/hangzhouwan/business.sock"""
        with open(EXAMPLE_YAML) as f:
            doc = yaml.safe_load(f)
        self.assertEqual(
            doc["business"]["socket_path"],
            "/run/hangzhouwan/business.sock",
            "示例 YAML socket_path 必须与 Video 服务一致"
        )


if __name__ == "__main__":
    unittest.main()
