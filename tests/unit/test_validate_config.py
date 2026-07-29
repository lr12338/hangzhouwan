# -*- coding: utf-8 -*-
"""配置校验单元测试（中文）。覆盖：YAML 语法、必填字段、环境变量缺失、
数值范围、重复 stream ID、帧率非法、队列必须为 1、模型文件不存在、
production 环境拒绝示例值。"""
import os
import sys
import copy
import unittest
import importlib.util

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
HAS_PYYAML = importlib.util.find_spec("yaml") is not None
if HAS_PYYAML:
    import yaml
    from tools.validate_config import validate, load_and_validate
else:
    yaml = None
    validate = load_and_validate = None

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLE = os.path.normpath(os.path.join(HERE, "..", "..", "config", "application.example.yaml"))


def load_example():
    with open(EXAMPLE, "r", encoding="utf-8") as f:
        return yaml.safe_load(f)


@unittest.skipUnless(HAS_PYYAML, "未安装 PyYAML，跳过配置 YAML 校验测试")
class ValidateConfigTest(unittest.TestCase):
    def test_example_yaml_syntax_ok(self):
        errors, warnings, data = load_and_validate(EXAMPLE)
        self.assertEqual([], errors, msg=str(errors))
        self.assertIsInstance(data, dict)

    def test_missing_required_field(self):
        d = load_example()
        del d["application"]
        errors, _ = validate(d)
        self.assertTrue(any("application" in e for e in errors))

    def test_duplicate_stream_id(self):
        d = load_example()
        d["streams"][1]["id"] = "A"
        errors, _ = validate(d)
        self.assertTrue(any("重复" in e for e in errors))

    def test_illegal_fps(self):
        d = load_example()
        d["streams"][0]["inference_fps"] = 0
        d["streams"][0]["output_fps"] = 100
        errors, _ = validate(d)
        self.assertTrue(any("帧率非法" in e for e in errors))

    def test_queue_must_be_one(self):
        d = load_example()
        d["runtime"]["frame_queue_size"] = 2
        errors, _ = validate(d)
        self.assertTrue(any("队列必须为 1" in e for e in errors))

    def test_numeric_range_confidence(self):
        d = load_example()
        d["inference"]["confidence_threshold"] = 1.5
        errors, _ = validate(d)
        self.assertTrue(any("confidence_threshold" in e for e in errors))

    def test_model_file_missing_warning(self):
        d = load_example()
        d["inference"]["model_path"] = "/nonexistent/best.onnx"
        errors, warnings = validate(d)
        self.assertEqual([], errors)
        self.assertTrue(any("不存在" in w for w in warnings))

    def test_env_var_missing_strict(self):
        d = load_example()
        d["streams"][0]["enabled"] = True
        environ = {}  # 不注入任何环境变量
        errors, _ = validate(d, env_strict=True, environ=environ)
        self.assertTrue(any("STREAM_A_INPUT_URL" in e for e in errors))

    def test_production_rejects_example_values(self):
        d = load_example()
        d["application"]["environment"] = "production"
        # 示例默认 disabled + 空 coordinate_model + 无 env，production 应报错
        environ = {}
        errors, _ = validate(d, environ=environ)
        joined = " ".join(errors)
        self.assertTrue(
            "model_path" in joined or "coordinate_model" in joined or "production" in joined,
            msg=str(errors))

    def test_production_requires_bmrt_backend(self):
        d = load_example()
        d["application"]["environment"] = "production"
        d["inference"]["backend"] = "onnxruntime"
        d["inference"]["model_path"] = "/data/best.bmodel"
        errors, _ = validate(d)
        self.assertTrue(any("bmrt" in e for e in errors))


if __name__ == "__main__":
    unittest.main()
