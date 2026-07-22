// -*- coding: utf-8 -*-
// =============================================================================
// ApplicationConfig：C++ 与 Python 共同读取的唯一权威配置模块（生产收口）。
//
// 读取 /etc/hangzhouwan/application.yaml（路径可由 --config 覆盖）。
// 内置轻量 YAML 子集解析器（不依赖外部库），处理本配置 schema 所需的：
//   嵌套 map、list-of-map、list-of-scalar、内联数组、标量。
//
// 配置必须包含：
//   bmodel 路径与 SHA256、A/B 启用状态、RTSP/RTMP 环境变量名、每路 output_fps /
//   inference_fps、bitrate、GOP、jitter buffer、conf/iou、禁区、coordinate 模式、
//   坐标模型、AIS 匹配阈值、business socket、日志和磁盘阈值、健康门禁。
//
// production 缺失关键项时必须启动失败（validate 返回 false）。
// 环境变量注入 RTSP/RTMP/MQTT 等敏感值，URL 不入配置文件/命令行/日志。
// =============================================================================
#ifndef HZW_CONFIG_APPLICATION_CONFIG_H
#define HZW_CONFIG_APPLICATION_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>
#include "video/detection_region_filter.h"  // Rect, Point

namespace hzw {

// ---------------------------------------------------------------------------
// YAML 子集解析器：将 YAML 文本解析为通用值树。
// ---------------------------------------------------------------------------
struct YamlValue {
  enum class Type { Null, Scalar, List, Map };
  Type type = Type::Null;
  std::string scalar;
  std::vector<YamlValue> list;
  std::vector<std::pair<std::string, YamlValue>> map;

  bool is_scalar() const { return type == Type::Scalar; }
  bool is_list() const { return type == Type::List; }
  bool is_map() const { return type == Type::Map; }

  const YamlValue* find(const std::string& key) const;
  std::string get_str(const std::string& key, const std::string& def = "") const;
  int get_int(const std::string& key, int def = 0) const;
  double get_dbl(const std::string& key, double def = 0.0) const;
  bool get_bool(const std::string& key, bool def = false) const;
};

// 解析 YAML 文本。失败返回 false，err 给出原因。
bool parse_yaml(const std::string& text, YamlValue& root, std::string& err);

// ---------------------------------------------------------------------------
// 单路流配置
// ---------------------------------------------------------------------------
struct StreamConfig {
  std::string id;                       // "A" | "B"
  bool enabled = false;
  std::string input_url_env;            // RTSP 输入 URL 环境变量名
  std::string output_url_env;           // RTMP 输出 URL 环境变量名
  std::string coordinate_model_path;    // 坐标模型路径
  int output_fps = 10;
  int inference_fps = 5;
  int bitrate_kbps = 800;
  int gop = 20;
  int jitter_buffer_size = 5;
  int result_ttl_ms = 1000;
  double camera_param = 0.0;
  float conf = 0.1f;
  float iou = 0.1f;
  std::vector<Rect> forbidden_rectangles;
  std::vector<std::vector<Point>> forbidden_polygons;
};

// ---------------------------------------------------------------------------
// 全局应用配置
// ---------------------------------------------------------------------------
struct ApplicationConfig {
  // application
  std::string environment = "development";   // development | staging | production
  std::string log_level = "INFO";

  // inference
  std::string bmodel_path;
  std::string bmodel_sha256;                 // 期望 SHA256（可空，非空时校验）
  int input_width = 640;
  int input_height = 640;
  float conf = 0.1f;
  float iou = 0.1f;
  int device = 0;

  // business
  std::string coordinate_mode = "sklearn";   // sklearn | numpy | mock | off
  std::string model_a_path;
  std::string model_b_path;
  int reference_width = 2560;
  int reference_height = 1440;
  std::string business_socket = "/run/hangzhouwan/business.sock";
  int request_timeout_ms = 30;
  int max_connections = 8;
  bool enable_evidence_recording = false;
  int ais_max_distance_m_a = 500;
  int ais_max_distance_m_b = 500;
  int ais_max_extrapolation_sec = 120;

  // mqtt
  std::string mqtt_host_env = "AIS_MQTT_HOST";
  int mqtt_port = 1883;
  std::string mqtt_client_id_env = "AIS_MQTT_CLIENT_ID";
  std::string mqtt_username_env = "AIS_MQTT_USERNAME";
  std::string mqtt_password_env = "AIS_MQTT_PASSWORD";
  std::vector<std::string> mqtt_topics;
  int mqtt_keepalive = 60;
  int mqtt_reconnect_sec = 5;

  // runtime
  int frame_queue_size = 1;
  int reconnect_initial_seconds = 2;
  int reconnect_max_seconds = 30;
  std::string decoder = "h264_bm";
  std::string encoder = "h264_bm";
  std::string preprocess = "bmcv";
  std::string draw_mode = "bmcv";

  // health
  int health_check_interval_seconds = 10;
  int stream_healthy_fps = 7;
  int stream_degraded_fps = 5;

  // logging / disk
  int video_log_max_size_mb = 50;
  int video_log_max_files = 10;
  int sidecar_log_max_size_mb = 50;
  int sidecar_log_max_files = 10;
  int jsonl_max_size_mb = 50;
  int jsonl_max_files = 10;
  int disk_threshold_mb = 500;

  // streams
  std::vector<StreamConfig> streams;

  // 解析 application.yaml 文件。失败返回 false，err 给出原因。
  bool load(const std::string& path, std::string& err);

  // 从 YAML 值树填充配置。
  bool from_yaml(const YamlValue& root, std::string& err);

  // 校验配置合法性。production 缺失关键项时返回 false。
  bool validate(std::string& err) const;

  // 按 id 查找流配置，未找到返回 nullptr。
  const StreamConfig* find_stream(const std::string& id) const;

  // 从环境变量解析实际 URL（不入配置/日志）。
  std::string resolve_env(const std::string& env_name) const;
};

}  // namespace hzw

#endif  // HZW_CONFIG_APPLICATION_CONFIG_H
