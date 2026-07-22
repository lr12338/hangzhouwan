// -*- coding: utf-8 -*-
#include "business/business_enrichment_client.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace hzw {

namespace {
// 极简 JSON 字符串转义
std::string escape_json(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out;
}

// 极简 JSON 值提取（查找 "key":value 模式）
std::string extract_string(const std::string& json, const std::string& key) {
  std::string pat = "\"" + key + "\":\"";
  auto pos = json.find(pat);
  if (pos == std::string::npos) return "";
  pos += pat.size();
  auto end = json.find('"', pos);
  if (end == std::string::npos) return "";
  return json.substr(pos, end - pos);
}

double extract_double(const std::string& json, const std::string& key) {
  std::string pat = "\"" + key + "\":";
  auto pos = json.find(pat);
  if (pos == std::string::npos) return 0.0;
  pos += pat.size();
  try { return std::stod(json.substr(pos)); }
  catch (...) { return 0.0; }
}

bool extract_bool(const std::string& json, const std::string& key) {
  std::string pat = "\"" + key + "\":";
  auto pos = json.find(pat);
  if (pos == std::string::npos) return false;
  pos += pat.size();
  return json.substr(pos, 4) == "true";
}

int extract_int(const std::string& json, const std::string& key) {
  return static_cast<int>(extract_double(json, key));
}

int64_t extract_int64(const std::string& json, const std::string& key) {
  std::string pat = "\"" + key + "\":";
  auto pos = json.find(pat);
  if (pos == std::string::npos) return 0;
  pos += pat.size();
  try { return static_cast<int64_t>(std::stoll(json.substr(pos))); }
  catch (...) { return 0; }
}

// 简单分割 results 数组中的每个对象
std::vector<std::string> split_results(const std::string& json) {
  std::vector<std::string> results;
  auto start = json.find("\"results\":[");
  if (start == std::string::npos) return results;
  start += 10;  // 指向 '['
  int depth = 0;
  size_t obj_start = std::string::npos;
  for (size_t i = start; i < json.size(); ++i) {
    if (json[i] == '{') {
      if (depth == 0) obj_start = i;
      ++depth;
    } else if (json[i] == '}') {
      --depth;
      if (depth == 0 && obj_start != std::string::npos) {
        results.push_back(json.substr(obj_start, i - obj_start + 1));
        obj_start = std::string::npos;
      }
    }
  }
  return results;
}
}  // namespace

BusinessEnrichmentClient::BusinessEnrichmentClient() = default;
BusinessEnrichmentClient::~BusinessEnrichmentClient() {
  if (fd_ >= 0) ::close(fd_);
}

bool BusinessEnrichmentClient::connect(const std::string& socket_path) {
  socket_path_ = socket_path;
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd_ < 0) return false;
  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  // 非阻塞连接 + 超时
  int flags = fcntl(fd_, F_GETFL, 0);
  fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
  int rc = ::connect(fd_, (struct sockaddr*)&addr, sizeof(addr));
  if (rc < 0 && errno != EINPROGRESS) {
    ::close(fd_); fd_ = -1;
    return false;
  }
  if (rc != 0) {
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd_, &wset);
    struct timeval tv{1, 0};  // 1 秒连接超时
    rc = ::select(fd_ + 1, nullptr, &wset, nullptr, &tv);
    if (rc <= 0) { ::close(fd_); fd_ = -1; return false; }
    int err = 0; socklen_t len = sizeof(err);
    getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) { ::close(fd_); fd_ = -1; return false; }
  }
  fcntl(fd_, F_SETFL, flags);  // 恢复阻塞模式
  // 设置发送/接收超时
  struct timeval tv{0, 200000};  // 200ms（初始默认，enrich 会覆盖）
  setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return true;
}

bool BusinessEnrichmentClient::enrich(const std::string& stream_id,
                                      int64_t frame_sequence,
                                      int image_width, int image_height,
                                      const std::vector<DetectionBox>& detections,
                                      std::vector<BusinessResult>& results,
                                      int timeout_ms) {
  results.clear();
  if (fd_ < 0) {
    // 尝试重连
    if (!connect(socket_path_)) {
      if (state_ != EnrichmentState::DETECTION_ONLY) {
        state_ = EnrichmentState::DETECTION_ONLY;
        ++degrade_count_;
      }
      return false;
    }
  }

  // 构造 JSON 请求
  std::ostringstream oss;
  oss << "{\"stream_id\":\"" << escape_json(stream_id)
      << "\",\"frame_sequence\":" << frame_sequence
      << ",\"image_width\":" << image_width
      << ",\"image_height\":" << image_height
      << ",\"detections\":[";
  for (size_t i = 0; i < detections.size(); ++i) {
    const auto& d = detections[i];
    if (i > 0) oss << ",";
    oss << "{\"detection_id\":" << d.detection_id
        << ",\"score\":" << d.score
        << ",\"x1\":" << d.x1 << ",\"y1\":" << d.y1
        << ",\"x2\":" << d.x2 << ",\"y2\":" << d.y2 << "}";
  }
  oss << "]}";
  std::string req = oss.str() + "\n";

  // 设置超时
  struct timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // 发送
  ssize_t sent = ::send(fd_, req.data(), req.size(), MSG_NOSIGNAL);
  if (sent < 0) {
    ::close(fd_); fd_ = -1;
    if (state_ != EnrichmentState::DETECTION_ONLY) {
      state_ = EnrichmentState::DETECTION_ONLY;
      ++degrade_count_;
    }
    return false;
  }

  // 接收（读到换行或超时）
  std::string resp;
  char buf[65536];
  while (resp.find('\n') == std::string::npos) {
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n <= 0) {
      if (n == 0 || errno != EINTR) {
        ::close(fd_); fd_ = -1;
        if (state_ != EnrichmentState::DETECTION_ONLY) {
          state_ = EnrichmentState::DETECTION_ONLY;
          ++degrade_count_;
        }
        return false;
      }
    } else {
      resp.append(buf, n);
      if (resp.size() > 1024 * 1024) break;  // 防止无界增长
    }
  }

  // 解析结果
  auto parts = split_results(resp);
  bool any_coord = false;
  bool any_match = false;
  for (const auto& part : parts) {
    BusinessResult r;
    r.detection_id = extract_int(part, "detection_id");
    r.longitude = extract_double(part, "longitude");
    r.latitude = extract_double(part, "latitude");
    r.coordinate_valid = extract_bool(part, "coordinate_valid");
    r.ais_matched = extract_bool(part, "ais_matched");
    r.mmsi = extract_string(part, "mmsi");
    r.ship_name = extract_string(part, "ship_name");
    r.speed = extract_double(part, "speed");
    r.course = extract_double(part, "course");
    r.ais_distance_km = extract_double(part, "ais_distance_km");
    r.ais_age_seconds = extract_int(part, "ais_age_seconds");
    r.ais_age_ms = extract_int64(part, "ais_age_ms");
    r.match_score = extract_double(part, "match_score");
    r.reject_reason = extract_string(part, "reject_reason");
    r.extrapolated = extract_bool(part, "extrapolated");
    r.ais_lon = extract_double(part, "ais_lon");
    r.ais_lat = extract_double(part, "ais_lat");
    r.ais_lon_aligned = extract_double(part, "ais_lon_aligned");
    r.ais_lat_aligned = extract_double(part, "ais_lat_aligned");
    r.score = static_cast<float>(extract_double(part, "score"));
    if (r.coordinate_valid) any_coord = true;
    if (r.ais_matched) any_match = true;
    results.push_back(r);
  }

  // 更新状态
  EnrichmentState new_state = EnrichmentState::DETECTION_ONLY;
  if (any_match) new_state = EnrichmentState::FULL;
  else if (any_coord) new_state = EnrichmentState::COORD_ONLY;
  if (new_state != state_) {
    if (state_ == EnrichmentState::DETECTION_ONLY) ++recover_count_;
    state_ = new_state;
  }
  return true;
}

}  // namespace hzw
