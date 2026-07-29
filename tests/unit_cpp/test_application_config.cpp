// -*- coding: utf-8 -*-
// ApplicationConfig + YAML 子集解析器单元测试（纯逻辑，无需硬件）。
#include <cassert>
#include <cstdio>
#include <string>
#include "config/application_config.h"

using namespace hzw;

static int failures = 0;
#define CHECK(cond) do { if(!(cond)){ ++failures; std::fprintf(stderr, "FAIL: %s @%d\n", #cond, __LINE__); } } while(0)

static const char* SAMPLE_YAML = R"(
application:
  environment: production
  log_level: INFO

inference:
  model_path: "/opt/hangzhouwan/current/models/yolov7.bmodel"
  bmodel_sha256: "abc123"
  input_width: 640
  input_height: 640
  confidence_threshold: 0.15
  iou_threshold: 0.2

streams:
  - id: A
    enabled: true
    input_url_env: STREAM_A_INPUT_URL
    output_url_env: STREAM_A_OUTPUT_URL
    coordinate_model: "/opt/models/a.pkl"
    output_fps: 10
    inference_fps: 5
    output_width: 1280
    output_height: 720
    output_bitrate_kbps: 1200
    gop: 20
    jitter_buffer_size: 5
    forbidden_rectangles: [[1480, 0, 2560, 630], [247, 855, 275, 888]]
    forbidden_polygons: [[[0, 0], [0, 640], [710, 620]]]
  - id: B
    enabled: true
    input_url_env: STREAM_B_INPUT_URL
    output_url_env: STREAM_B_OUTPUT_URL
    coordinate_model: "/opt/models/b.pkl"
    output_fps: 10
    inference_fps: 5
    forbidden_rectangles: []
    forbidden_polygons: []

business:
  coordinate_mode: sklearn
  model_a_path: "/opt/models/a.pkl"
  model_b_path: "/opt/models/b.pkl"
  socket_path: "/run/hangzhouwan/business.sock"
  ais_max_distance_m:
    A: 500
    B: 600

mqtt:
  host_env: AIS_MQTT_HOST
  port: 1883
  topics: ["upAIS/base_2250", "upAIS/base_2251"]

logging:
  disk_threshold_mb: 500
)";

static void test_yaml_parser() {
  YamlValue root;
  std::string err;
  bool ok = parse_yaml(SAMPLE_YAML, root, err);
  CHECK(ok);
  CHECK(root.is_map());
  const YamlValue* app = root.find("application");
  CHECK(app != nullptr);
  CHECK(app->get_str("environment") == "production");

  const YamlValue* streams = root.find("streams");
  CHECK(streams != nullptr);
  CHECK(streams->is_list());
  CHECK(streams->list.size() == 2);
  CHECK(streams->list[0].get_str("id") == "A");
  CHECK(streams->list[0].get_bool("enabled") == true);

  const YamlValue* rects = streams->list[0].find("forbidden_rectangles");
  CHECK(rects != nullptr);
  CHECK(rects->is_list());
  CHECK(rects->list.size() == 2);
  CHECK(rects->list[0].is_list());
  CHECK(rects->list[0].list.size() == 4);
  CHECK(rects->list[0].list[0].scalar == "1480");
  CHECK(rects->list[0].list[3].scalar == "630");

  const YamlValue* polys = streams->list[0].find("forbidden_polygons");
  CHECK(polys != nullptr);
  CHECK(polys->is_list());
  CHECK(polys->list.size() == 1);
  CHECK(polys->list[0].is_list());
  CHECK(polys->list[0].list.size() == 3);
  CHECK(polys->list[0].list[0].is_list());
  CHECK(polys->list[0].list[0].list[0].scalar == "0");

  const YamlValue* biz = root.find("business");
  const YamlValue* dist = biz->find("ais_max_distance_m");
  CHECK(dist != nullptr);
  CHECK(dist->is_map());
  CHECK(dist->get_int("A") == 500);
  CHECK(dist->get_int("B") == 600);

  const YamlValue* mqtt = root.find("mqtt");
  const YamlValue* topics = mqtt->find("topics");
  CHECK(topics != nullptr);
  CHECK(topics->is_list());
  CHECK(topics->list.size() == 2);
  CHECK(topics->list[0].scalar == "upAIS/base_2250");
}

static void test_config_load() {
  YamlValue root;
  std::string err;
  parse_yaml(SAMPLE_YAML, root, err);
  ApplicationConfig cfg;
  bool ok = cfg.from_yaml(root, err);
  CHECK(ok);
  CHECK(cfg.environment == "production");
  CHECK(cfg.bmodel_path == "/opt/hangzhouwan/current/models/yolov7.bmodel");
  CHECK(cfg.bmodel_sha256 == "abc123");
  CHECK(cfg.coordinate_mode == "sklearn");
  CHECK(cfg.business_socket == "/run/hangzhouwan/business.sock");
  CHECK(cfg.ais_max_distance_m_a == 500);
  CHECK(cfg.ais_max_distance_m_b == 600);
  CHECK(cfg.streams.size() == 2);

  const StreamConfig* a = cfg.find_stream("A");
  CHECK(a != nullptr);
  CHECK(a->enabled == true);
  CHECK(a->input_url_env == "STREAM_A_INPUT_URL");
  CHECK(a->output_fps == 10);
  CHECK(a->forbidden_rectangles.size() == 2);
  CHECK(a->forbidden_rectangles[0].x1 == 1480);
  CHECK(a->forbidden_rectangles[0].y2 == 630);
  CHECK(a->forbidden_polygons.size() == 1);
  CHECK(a->forbidden_polygons[0].size() == 3);
  CHECK(a->forbidden_polygons[0][0].x == 0);
  CHECK(a->conf == 0.15f);
  CHECK(a->iou == 0.2f);

  const StreamConfig* b = cfg.find_stream("B");
  CHECK(b != nullptr);
  CHECK(b->forbidden_rectangles.empty());

  // resolve_env（无环境变量时返回空）
  CHECK(cfg.resolve_env("NONEXISTENT_VAR_XYZ") == "");
  // extra_frame_buffer_num 默认值（SAMPLE_YAML 无 runtime 段）
  CHECK(cfg.extra_frame_buffer_num == 20);
}

static void test_production_rejects_mock() {
  const char* mock_yaml = R"(
application:
  environment: production
inference:
  model_path: "/opt/m.bmodel"
streams:
  - id: A
    enabled: true
    input_url_env: STREAM_A_INPUT_URL
    output_url_env: STREAM_A_OUTPUT_URL
    coordinate_model: "/opt/a.pkl"
    output_fps: 10
    inference_fps: 5
business:
  coordinate_mode: mock
)";
  YamlValue root;
  std::string err;
  parse_yaml(mock_yaml, root, err);
  ApplicationConfig cfg;
  bool ok = cfg.from_yaml(root, err);
  CHECK(!ok);
  CHECK(err.find("mock") != std::string::npos);
}

static void test_production_missing_model() {
  const char* yaml = R"(
application:
  environment: production
inference:
  model_path: "/opt/m.bmodel"
streams:
  - id: A
    enabled: true
    input_url_env: STREAM_A_INPUT_URL
    output_url_env: STREAM_A_OUTPUT_URL
    output_fps: 10
    inference_fps: 5
business:
  coordinate_mode: sklearn
)";
  YamlValue root;
  std::string err;
  parse_yaml(yaml, root, err);
  ApplicationConfig cfg;
  bool ok = cfg.from_yaml(root, err);
  CHECK(!ok);
  CHECK(err.find("coordinate_model") != std::string::npos);
}

static void test_production_rejects_old_output_spec() {
  const char* yaml = R"(
application:
  environment: production
inference:
  model_path: "/opt/m.bmodel"
streams:
  - id: A
    enabled: true
    input_url_env: STREAM_A_INPUT_URL
    output_url_env: STREAM_A_OUTPUT_URL
    coordinate_model: "/opt/a.pkl"
    output_width: 960
    output_height: 540
    output_fps: 10
    inference_fps: 5
    output_bitrate_kbps: 800
business:
  coordinate_mode: sklearn
)";
  YamlValue root;
  std::string err;
  parse_yaml(yaml, root, err);
  ApplicationConfig cfg;
  bool ok = cfg.from_yaml(root, err);
  CHECK(!ok);
  CHECK(err.find("1280x720") != std::string::npos);
}

static void test_extra_frame_buffer_num() {
  // 配置化：runtime.extra_frame_buffer_num 覆盖默认值。
  const char* yaml = R"(
application:
  environment: development
inference:
  model_path: "/opt/m.bmodel"
runtime:
  extra_frame_buffer_num: 12
streams:
  - id: A
    enabled: false
    input_url_env: STREAM_A_INPUT_URL
    output_url_env: STREAM_A_OUTPUT_URL
    output_fps: 10
    inference_fps: 5
)";
  YamlValue root;
  std::string err;
  parse_yaml(yaml, root, err);
  ApplicationConfig cfg;
  bool ok = cfg.from_yaml(root, err);
  CHECK(ok);
  CHECK(cfg.extra_frame_buffer_num == 12);

  // 非法值（< 1）必须被拒绝。
  const char* bad_yaml = R"(
application:
  environment: development
inference:
  model_path: "/opt/m.bmodel"
runtime:
  extra_frame_buffer_num: 0
streams:
  - id: A
    enabled: false
    input_url_env: STREAM_A_INPUT_URL
    output_url_env: STREAM_A_OUTPUT_URL
    output_fps: 10
    inference_fps: 5
)";
  YamlValue root2;
  parse_yaml(bad_yaml, root2, err);
  ApplicationConfig cfg2;
  bool ok2 = cfg2.from_yaml(root2, err);
  CHECK(!ok2);
  CHECK(err.find("extra_frame_buffer_num") != std::string::npos);
}

static void test_production_requires_industrial_health_fps() {
  const char* yaml = R"(
application:
  environment: production
inference:
  model_path: "/opt/m.bmodel"
health:
  stream_healthy_fps: 7
streams:
  - id: A
    enabled: true
    input_url_env: STREAM_A_INPUT_URL
    output_url_env: STREAM_A_OUTPUT_URL
    coordinate_model: "/opt/a.pkl"
    output_width: 1280
    output_height: 720
    output_fps: 10
    inference_fps: 5
    output_bitrate_kbps: 1200
    gop: 20
business:
  coordinate_mode: sklearn
  socket_path: "/run/hangzhouwan/business.sock"
)";
  YamlValue root;
  std::string err;
  parse_yaml(yaml, root, err);
  ApplicationConfig cfg;
  bool ok = cfg.from_yaml(root, err);
  CHECK(!ok);
  CHECK(err.find("stream_healthy_fps") != std::string::npos);
}

int main() {
  test_yaml_parser();
  test_config_load();
  test_production_rejects_mock();
  test_production_missing_model();
  test_production_rejects_old_output_spec();
  test_extra_frame_buffer_num();
  test_production_requires_industrial_health_fps();
  if (failures == 0) {
    std::printf("OK application_config: all tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "FAIL application_config: %d failures\n", failures);
  return 1;
}
