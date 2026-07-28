// -*- coding: utf-8 -*-
#include "config/application_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hzw {

// ===========================================================================
// YAML 子集解析器实现
// ===========================================================================

namespace {

struct Line {
  int indent;
  std::string text;
};

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}

// 去除行内注释（# 前需为空白或行首，且不在引号内）。
std::string strip_comment(const std::string& s) {
  bool in_squote = false, in_dquote = false;
  bool in_bracket = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\'' && !in_dquote) in_squote = !in_squote;
    else if (c == '"' && !in_squote) in_dquote = !in_dquote;
    else if (c == '[' && !in_squote && !in_dquote) in_bracket = true;
    else if (c == ']' && !in_squote && !in_dquote) in_bracket = false;
    else if (c == '#' && !in_squote && !in_dquote && !in_bracket) {
      if (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t') {
        return s.substr(0, i);
      }
    }
  }
  return s;
}

std::vector<Line> preprocess(const std::string& text) {
  std::vector<Line> lines;
  std::istringstream iss(text);
  std::string raw;
  while (std::getline(iss, raw)) {
    std::string no_comment = strip_comment(raw);
    size_t indent = 0;
    while (indent < no_comment.size() && no_comment[indent] == ' ') ++indent;
    std::string content = trim(no_comment.substr(indent));
    if (content.empty()) continue;
    // tab 不允许作为缩进
    lines.push_back({static_cast<int>(indent), content});
  }
  return lines;
}

// 在 text 中查找顶级 key:value 分隔符（不在引号/括号内的 ": " 或行尾 ":"）。
// 返回分隔位置（冒号位置），找不到返回 npos。
size_t find_kv_colon(const std::string& text) {
  bool in_squote = false, in_dquote = false;
  int bracket = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\'' && !in_dquote) in_squote = !in_squote;
    else if (c == '"' && !in_squote) in_dquote = !in_dquote;
    else if (!in_squote && !in_dquote) {
      if (c == '[') ++bracket;
      else if (c == ']') --bracket;
      else if (c == ':' && bracket == 0) {
        if (i + 1 >= text.size() || text[i + 1] == ' ') return i;
      }
    }
  }
  return std::string::npos;
}

// 解析内联标量（去引号、转 int/float/bool/字符串）。
YamlValue parse_scalar(const std::string& raw) {
  YamlValue v;
  v.type = YamlValue::Type::Scalar;
  std::string s = trim(raw);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    v.scalar = s.substr(1, s.size() - 2);
    return v;
  }
  if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
    v.scalar = s.substr(1, s.size() - 2);
    return v;
  }
  v.scalar = s;
  return v;
}

// 解析内联数组 [a, b, c] 或 [[x,y],[z,w]]。
YamlValue parse_inline_array(const std::string& s);

YamlValue parse_inline(const std::string& raw) {
  std::string s = trim(raw);
  if (s.empty()) {
    YamlValue v;
    v.type = YamlValue::Type::Null;
    return v;
  }
  if (s.front() == '[') return parse_inline_array(s);
  return parse_scalar(s);
}

YamlValue parse_inline_array(const std::string& s) {
  YamlValue v;
  v.type = YamlValue::Type::List;
  // s 以 '[' 开头。找到匹配的 ']'。
  if (s.empty() || s.front() != '[') return v;
  int depth = 0;
  size_t start = 1;
  bool in_squote = false, in_dquote = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\'' && !in_dquote) in_squote = !in_squote;
    else if (c == '"' && !in_squote) in_dquote = !in_dquote;
    else if (!in_squote && !in_dquote) {
      if (c == '[') {
        if (depth == 0) start = i + 1;
        ++depth;
      } else if (c == ']') {
        --depth;
        if (depth == 0) {
          std::string elem = trim(s.substr(start, i - start));
          if (!elem.empty()) v.list.push_back(parse_inline(elem));
          break;
        }
      } else if (c == ',' && depth == 1) {
        std::string elem = trim(s.substr(start, i - start));
        if (!elem.empty()) v.list.push_back(parse_inline(elem));
        start = i + 1;
      }
    }
  }
  return v;
}

// 前向声明
YamlValue parse_node(std::vector<Line>& lines, int& idx, int indent);

YamlValue parse_map(std::vector<Line>& lines, int& idx, int indent) {
  YamlValue result;
  result.type = YamlValue::Type::Map;
  while (idx < static_cast<int>(lines.size())) {
    Line& l = lines[idx];
    if (l.indent < indent) break;
    if (l.indent > indent) break;
    if (!l.text.empty() && l.text[0] == '-') break;

    size_t colon = find_kv_colon(l.text);
    if (colon == std::string::npos) {
      // 不是 key:value，跳过
      ++idx;
      continue;
    }
    std::string key = trim(l.text.substr(0, colon));
    std::string val_str = trim(l.text.substr(colon + 1));
    ++idx;

    if (val_str.empty()) {
      // 子块跟随
      if (idx < static_cast<int>(lines.size()) && lines[idx].indent > indent) {
        YamlValue child = parse_node(lines, idx, lines[idx].indent);
        result.map.push_back({key, child});
      } else {
        YamlValue null_v;
        null_v.type = YamlValue::Type::Null;
        result.map.push_back({key, null_v});
      }
    } else {
      result.map.push_back({key, parse_inline(val_str)});
    }
  }
  return result;
}

YamlValue parse_sequence(std::vector<Line>& lines, int& idx, int indent) {
  YamlValue result;
  result.type = YamlValue::Type::List;
  while (idx < static_cast<int>(lines.size())) {
    Line& l = lines[idx];
    if (l.indent < indent) break;
    if (l.indent > indent) break;
    if (l.text.empty() || l.text[0] != '-') break;

    // 去掉 "-" 前缀
    std::string after = l.text.substr(1);
    size_t sp = after.find_first_not_of(' ');
    if (sp == std::string::npos) {
      // dash 后为空，子块跟随
      ++idx;
      if (idx < static_cast<int>(lines.size()) && lines[idx].indent > indent) {
        YamlValue child = parse_node(lines, idx, lines[idx].indent);
        result.list.push_back(child);
      } else {
        YamlValue null_v;
        null_v.type = YamlValue::Type::Null;
        result.list.push_back(null_v);
      }
    } else {
      std::string content = after.substr(sp);
      int content_col = indent + 1 + static_cast<int>(sp);
      // 检查是否为 key:value（map 元素）
      size_t colon = find_kv_colon(content);
      if (colon != std::string::npos) {
        // map 元素：将当前行改写为 content_col 缩进，然后 parse_map
        lines[idx].indent = content_col;
        lines[idx].text = content;
        YamlValue child = parse_map(lines, idx, content_col);
        result.list.push_back(child);
      } else {
        // 标量元素
        result.list.push_back(parse_inline(content));
        ++idx;
      }
    }
  }
  return result;
}

YamlValue parse_node(std::vector<Line>& lines, int& idx, int indent) {
  if (idx >= static_cast<int>(lines.size())) {
    YamlValue v;
    v.type = YamlValue::Type::Null;
    return v;
  }
  if (!lines[idx].text.empty() && lines[idx].text[0] == '-') {
    return parse_sequence(lines, idx, indent);
  }
  return parse_map(lines, idx, indent);
}

}  // namespace

bool parse_yaml(const std::string& text, YamlValue& root, std::string& err) {
  std::vector<Line> lines = preprocess(text);
  if (lines.empty()) {
    root.type = YamlValue::Type::Map;
    return true;
  }
  int idx = 0;
  root = parse_node(lines, idx, lines[0].indent);
  if (idx < static_cast<int>(lines.size())) {
    // 未完全消费 —— 宽容处理，不视为错误
  }
  return true;
}

// ===========================================================================
// YamlValue 查找辅助
// ===========================================================================

const YamlValue* YamlValue::find(const std::string& key) const {
  for (const auto& kv : map) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

std::string YamlValue::get_str(const std::string& key, const std::string& def) const {
  const YamlValue* v = find(key);
  if (!v || !v->is_scalar()) return def;
  return v->scalar;
}

int YamlValue::get_int(const std::string& key, int def) const {
  const YamlValue* v = find(key);
  if (!v || !v->is_scalar() || v->scalar.empty()) return def;
  try { return std::stoi(v->scalar); }
  catch (...) { return def; }
}

double YamlValue::get_dbl(const std::string& key, double def) const {
  const YamlValue* v = find(key);
  if (!v || !v->is_scalar() || v->scalar.empty()) return def;
  try { return std::stod(v->scalar); }
  catch (...) { return def; }
}

bool YamlValue::get_bool(const std::string& key, bool def) const {
  const YamlValue* v = find(key);
  if (!v || !v->is_scalar()) return def;
  const std::string& s = v->scalar;
  if (s == "true" || s == "True" || s == "TRUE") return true;
  if (s == "false" || s == "False" || s == "FALSE") return false;
  return def;
}

// ===========================================================================
// ApplicationConfig 加载与校验
// ===========================================================================

namespace {

int scalar_to_int(const YamlValue& v, int def = 0) {
  if (!v.is_scalar() || v.scalar.empty()) return def;
  try { return std::stoi(v.scalar); }
  catch (...) { return def; }
}

double scalar_to_dbl(const YamlValue& v, double def = 0.0) {
  if (!v.is_scalar() || v.scalar.empty()) return def;
  try { return std::stod(v.scalar); }
  catch (...) { return def; }
}

void parse_stream(const YamlValue& sv, StreamConfig& sc) {
  sc.id = sv.get_str("id", sc.id);
  sc.enabled = sv.get_bool("enabled", sc.enabled);
  sc.input_url_env = sv.get_str("input_url_env", sc.input_url_env);
  sc.output_url_env = sv.get_str("output_url_env", sc.output_url_env);
  sc.coordinate_model_path = sv.get_str("coordinate_model", sc.coordinate_model_path);
  sc.output_fps = sv.get_int("output_fps", sc.output_fps);
  sc.inference_fps = sv.get_int("inference_fps", sc.inference_fps);
  sc.output_width = sv.get_int("output_width", sc.output_width);
  sc.output_height = sv.get_int("output_height", sc.output_height);
  sc.bitrate_kbps = sv.get_int("output_bitrate_kbps", sv.get_int("bitrate_kbps", sc.bitrate_kbps));
  sc.gop = sv.get_int("gop", sc.gop);
  sc.jitter_buffer_size = sv.get_int("jitter_buffer_size", sc.jitter_buffer_size);
  sc.result_ttl_ms = sv.get_int("result_ttl_ms", sc.result_ttl_ms);
  sc.camera_param = sv.get_dbl("camera_param", sc.camera_param);
  // conf/iou：流级覆盖全局 inference 默认值
  sc.conf = static_cast<float>(sv.get_dbl("confidence_threshold", sv.get_dbl("conf", sc.conf)));
  sc.iou = static_cast<float>(sv.get_dbl("iou_threshold", sv.get_dbl("iou", sc.iou)));

  // forbidden_rectangles: [[x1,y1,x2,y2], ...]
  const YamlValue* rects = sv.find("forbidden_rectangles");
  if (rects && rects->is_list()) {
    for (const auto& r : rects->list) {
      if (r.is_list() && r.list.size() >= 4) {
        Rect rect;
        rect.x1 = scalar_to_int(r.list[0]);
        rect.y1 = scalar_to_int(r.list[1]);
        rect.x2 = scalar_to_int(r.list[2]);
        rect.y2 = scalar_to_int(r.list[3]);
        sc.forbidden_rectangles.push_back(rect);
      } else if (r.is_scalar()) {
        // 空数组元素，跳过
      }
    }
  }
  // forbidden_polygons: [[[x,y],...], ...]
  const YamlValue* polys = sv.find("forbidden_polygons");
  if (polys && polys->is_list()) {
    for (const auto& p : polys->list) {
      if (p.is_list()) {
        std::vector<Point> poly;
        for (const auto& pt : p.list) {
          if (pt.is_list() && pt.list.size() >= 2) {
            Point point;
            point.x = scalar_to_int(pt.list[0]);
            point.y = scalar_to_int(pt.list[1]);
            poly.push_back(point);
          }
        }
        if (!poly.empty()) sc.forbidden_polygons.push_back(poly);
      }
    }
  }
}

}  // namespace

bool ApplicationConfig::load(const std::string& path, std::string& err) {
  std::ifstream f(path);
  if (!f.is_open()) {
    err = "无法打开配置文件: " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();

  YamlValue root;
  if (!parse_yaml(text, root, err)) return false;
  return from_yaml(root, err);
}

bool ApplicationConfig::from_yaml(const YamlValue& root, std::string& err) {
  // application
  const YamlValue* app = root.find("application");
  if (app && app->is_map()) {
    environment = app->get_str("environment", environment);
    log_level = app->get_str("log_level", log_level);
  }
  // inference
  const YamlValue* inf = root.find("inference");
  if (inf && inf->is_map()) {
    bmodel_path = inf->get_str("model_path", inf->get_str("bmodel_path", bmodel_path));
    bmodel_sha256 = inf->get_str("bmodel_sha256", bmodel_sha256);
    input_width = inf->get_int("input_width", input_width);
    input_height = inf->get_int("input_height", input_height);
    conf = static_cast<float>(inf->get_dbl("confidence_threshold", inf->get_dbl("conf", conf)));
    iou = static_cast<float>(inf->get_dbl("iou_threshold", inf->get_dbl("iou", iou)));
    device = inf->get_int("device", device);
  }
  // business
  const YamlValue* biz = root.find("business");
  if (biz && biz->is_map()) {
    coordinate_mode = biz->get_str("coordinate_mode", coordinate_mode);
    model_a_path = biz->get_str("model_a_path", model_a_path);
    model_b_path = biz->get_str("model_b_path", model_b_path);
    reference_width = biz->get_int("reference_width", reference_width);
    reference_height = biz->get_int("reference_height", reference_height);
    business_socket = biz->get_str("socket_path", business_socket);
    request_timeout_ms = biz->get_int("request_timeout_ms", request_timeout_ms);
    max_connections = biz->get_int("max_connections", max_connections);
    enable_evidence_recording = biz->get_bool("enable_evidence_recording", enable_evidence_recording);
    ais_max_extrapolation_sec = biz->get_int("ais_max_extrapolation_sec", ais_max_extrapolation_sec);
    const YamlValue* dist = biz->find("ais_max_distance_m");
    if (dist && dist->is_map()) {
      ais_max_distance_m_a = dist->get_int("A", ais_max_distance_m_a);
      ais_max_distance_m_b = dist->get_int("B", ais_max_distance_m_b);
    }
  }
  // mqtt
  const YamlValue* mqtt = root.find("mqtt");
  if (mqtt && mqtt->is_map()) {
    mqtt_host_env = mqtt->get_str("host_env", mqtt_host_env);
    mqtt_port = mqtt->get_int("port", mqtt_port);
    mqtt_client_id_env = mqtt->get_str("client_id_env", mqtt_client_id_env);
    mqtt_username_env = mqtt->get_str("username_env", mqtt_username_env);
    mqtt_password_env = mqtt->get_str("password_env", mqtt_password_env);
    mqtt_keepalive = mqtt->get_int("keepalive", mqtt_keepalive);
    mqtt_reconnect_sec = mqtt->get_int("reconnect_sec", mqtt_reconnect_sec);
    const YamlValue* topics = mqtt->find("topics");
    if (topics && topics->is_list()) {
      mqtt_topics.clear();
      for (const auto& t : topics->list) {
        if (t.is_scalar() && !t.scalar.empty()) mqtt_topics.push_back(t.scalar);
      }
    }
  }
  // runtime
  const YamlValue* rt = root.find("runtime");
  if (rt && rt->is_map()) {
    frame_queue_size = rt->get_int("frame_queue_size", frame_queue_size);
    reconnect_initial_seconds = rt->get_int("reconnect_initial_seconds", reconnect_initial_seconds);
    reconnect_max_seconds = rt->get_int("reconnect_max_seconds", reconnect_max_seconds);
    extra_frame_buffer_num = rt->get_int("extra_frame_buffer_num", extra_frame_buffer_num);
  }
  // health
  const YamlValue* hlth = root.find("health");
  if (hlth && hlth->is_map()) {
    health_check_interval_seconds = hlth->get_int("check_interval_seconds", health_check_interval_seconds);
    stream_healthy_fps = hlth->get_int("stream_healthy_fps", stream_healthy_fps);
    stream_degraded_fps = hlth->get_int("stream_degraded_fps", stream_degraded_fps);
    inference_healthy_fps = hlth->get_int("inference_healthy_fps", inference_healthy_fps);
    frame_stale_seconds = hlth->get_int("frame_stale_seconds", frame_stale_seconds);
    stream_failed_seconds = hlth->get_int("stream_failed_seconds", stream_failed_seconds);
    reconnects_per_hour = hlth->get_int("reconnects_per_hour", reconnects_per_hour);
  }
  // logging
  const YamlValue* log = root.find("logging");
  if (log && log->is_map()) {
    video_log_max_size_mb = log->get_int("video_log_max_size_mb", video_log_max_size_mb);
    video_log_max_files = log->get_int("video_log_max_files", video_log_max_files);
    sidecar_log_max_size_mb = log->get_int("sidecar_log_max_size_mb", sidecar_log_max_size_mb);
    sidecar_log_max_files = log->get_int("sidecar_log_max_files", sidecar_log_max_files);
    jsonl_max_size_mb = log->get_int("jsonl_max_size_mb", jsonl_max_size_mb);
    jsonl_max_files = log->get_int("jsonl_max_files", jsonl_max_files);
    disk_threshold_mb = log->get_int("disk_threshold_mb", disk_threshold_mb);
  }
  const YamlValue* events = root.find("event_writer");
  if (events && events->is_map()) {
    event_directory = events->get_str("directory", event_directory);
    event_rotate_size_mb = events->get_int("rotate_size_mb", event_rotate_size_mb);
    event_rotate_seconds = events->get_int("rotate_seconds", event_rotate_seconds);
    event_retention_days = events->get_int("retention_days", event_retention_days);
    event_sync_seconds = events->get_int("sync_seconds", event_sync_seconds);
    event_queue_max_mb = events->get_int("queue_max_mb", event_queue_max_mb);
    event_disk_warn_mb = events->get_int("disk_warn_mb", event_disk_warn_mb);
    event_disk_stop_mb = events->get_int("disk_stop_mb", event_disk_stop_mb);
  }
  // streams
  const YamlValue* streams_v = root.find("streams");
  if (streams_v && streams_v->is_list()) {
    streams.clear();
    for (const auto& sv : streams_v->list) {
      if (sv.is_map()) {
        StreamConfig sc;
        // 流级 conf/iou 默认继承全局 inference 配置
        sc.conf = conf;
        sc.iou = iou;
        sc.bitrate_kbps = 1200;
        sc.gop = 20;
        sc.jitter_buffer_size = 5;
        parse_stream(sv, sc);
        streams.push_back(sc);
      }
    }
  }
  return validate(err);
}

bool ApplicationConfig::validate(std::string& err) const {
  if (environment != "development" && environment != "staging" && environment != "production") {
    err = "environment 必须为 development|staging|production，当前=" + environment;
    return false;
  }
  const bool prod = (environment == "production");

  if (coordinate_mode != "sklearn" && coordinate_mode != "numpy" &&
      coordinate_mode != "mock" && coordinate_mode != "off") {
    err = "coordinate_mode 非法: " + coordinate_mode;
    return false;
  }
  if (prod && (coordinate_mode == "mock" || coordinate_mode == "off")) {
    err = "production 禁止 mock/off coordinate_mode";
    return false;
  }

  if (bmodel_path.empty()) {
    err = "bmodel_path 未配置";
    return false;
  }

  if (streams.empty()) {
    err = "streams 未配置";
    return false;
  }

  if (extra_frame_buffer_num < 1) {
    err = "extra_frame_buffer_num 至少为 1";
    return false;
  }
  for (const auto& s : streams) {
    if (!s.enabled) continue;
    if (s.id.empty()) { err = "stream id 为空"; return false; }
    if (s.input_url_env.empty()) { err = "stream " + s.id + " input_url_env 未配置"; return false; }
    if (s.output_url_env.empty()) { err = "stream " + s.id + " output_url_env 未配置"; return false; }
    if (s.output_fps <= 0) { err = "stream " + s.id + " output_fps 非法"; return false; }
    if (s.inference_fps <= 0) { err = "stream " + s.id + " inference_fps 非法"; return false; }
    if (s.inference_fps > s.output_fps) {
      err = "stream " + s.id + " inference_fps 大于 output_fps";
      return false;
    }
    if (s.output_width < 320 || s.output_height < 240 ||
        (s.output_width % 2) != 0 || (s.output_height % 2) != 0) {
      err = "stream " + s.id + " 输出分辨率必须为不小于320x240的偶数";
      return false;
    }
    if (s.bitrate_kbps <= 0) { err = "stream " + s.id + " bitrate_kbps 非法"; return false; }
    if (s.gop <= 0) { err = "stream " + s.id + " gop 非法"; return false; }
    if (prod) {
      if (s.output_width != 1280 || s.output_height != 720 ||
          s.output_fps != 10 || s.bitrate_kbps != 1200 || s.gop != 20) {
        err = "production stream " + s.id +
              " 输出必须为1280x720@10fps/1200kbps/GOP20";
        return false;
      }
      if (coordinate_mode == "sklearn" || coordinate_mode == "numpy") {
        if (s.coordinate_model_path.empty()) {
          err = "production stream " + s.id + " coordinate_model 未配置";
          return false;
        }
      }
    }
  }

  // production 必须有 business socket
  if (prod && business_socket.empty()) {
    err = "production business_socket 未配置";
    return false;
  }
  if (event_directory.compare(0, 6, "/data/") != 0) {
    err = "event_writer.directory 必须位于 /data，禁止回退根分区";
    return false;
  }
  if (event_rotate_size_mb < 1 || event_rotate_seconds < 1 ||
      event_retention_days < 1 || event_sync_seconds < 1 ||
      event_queue_max_mb < 1 || event_disk_stop_mb < 1 ||
      event_disk_warn_mb <= event_disk_stop_mb) {
    err = "event_writer 参数非法";
    return false;
  }
  if (stream_healthy_fps < 1 || inference_healthy_fps < 1 ||
      frame_stale_seconds < 1 ||
      stream_failed_seconds <= frame_stale_seconds ||
      reconnects_per_hour < 0) {
    err = "health 阈值非法";
    return false;
  }

  return true;
}

const StreamConfig* ApplicationConfig::find_stream(const std::string& id) const {
  for (const auto& s : streams) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

std::string ApplicationConfig::resolve_env(const std::string& env_name) const {
  if (env_name.empty()) return "";
  const char* v = std::getenv(env_name.c_str());
  return v ? std::string(v) : std::string();
}

}  // namespace hzw
