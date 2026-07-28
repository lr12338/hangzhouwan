// -*- coding: utf-8 -*-
#include "monitoring/video_health_server.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <chrono>

namespace hzw {

namespace {

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

std::string stream_to_json(const std::string& key, const StreamHealthSnapshot& s) {
  std::ostringstream oss;
  oss << "\"" << key << "\":{"
      << "\"stream_id\":\"" << escape_json(s.stream_id) << "\""
      << ",\"level\":\"" << escape_json(s.level) << "\""
      << ",\"rtsp_connected\":" << (s.rtsp_connected ? "true" : "false")
      << ",\"rtmp_connected\":" << (s.rtmp_connected ? "true" : "false")
      << ",\"output_fps\":" << s.output_fps
      << ",\"inference_fps\":" << s.inference_fps
      << ",\"last_frame_time_ms\":" << s.last_frame_time_ms
      << ",\"rtsp_reconnects\":" << s.rtsp_reconnects
      << ",\"rtmp_reconnects\":" << s.rtmp_reconnects
      << ",\"reconnects_1h\":" << s.reconnects_1h
      << ",\"queue_length\":" << s.queue_length
      << ",\"e2e_p95_ms\":" << s.e2e_p95_ms
      << "}";
  return oss.str();
}

std::string event_writer_to_json(const std::string& key,
                                 const EventWriterHealthSnapshot& s) {
  std::ostringstream oss;
  oss << "\"" << key << "\":{"
      << "\"running\":" << (s.running ? "true" : "false")
      << ",\"write_enabled\":" << (s.write_enabled ? "true" : "false")
      << ",\"low_space_warning\":" << (s.low_space_warning ? "true" : "false")
      << ",\"queued_bytes\":" << s.queued_bytes
      << ",\"written_records\":" << s.written_records
      << ",\"dropped_records\":" << s.dropped_records
      << ",\"last_error\":\"" << escape_json(s.last_error) << "\"}";
  return oss.str();
}

std::string extract_version_field(const std::string& path, const std::string& field) {
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.find(field + ":") == 0) {
      return line.substr(field.size() + 1);
    }
    // 首行是版本名
    if (field == "version" && &line == &line) {
      // 第一行即版本
    }
  }
  return "";
}

std::string read_file_first_line(const std::string& path) {
  std::ifstream f(path);
  std::string line;
  if (std::getline(f, line)) return line;
  return "";
}

}  // namespace

VideoHealthServer::VideoHealthServer() = default;

VideoHealthServer::~VideoHealthServer() {
  stop();
}

bool VideoHealthServer::start(const std::string& socket_path) {
  socket_path_ = socket_path;

  // 确保目录存在
  std::string dir = socket_path;
  auto pos = dir.find_last_of('/');
  if (pos != std::string::npos) {
    dir = dir.substr(0, pos);
    ::mkdir(dir.c_str(), 0755);  // NOLINT
  }

  // 清理旧 Socket
  ::unlink(socket_path_.c_str());

  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return false;

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  // 监督器和维护任务以独立身份、共享 hangzhouwan 组访问健康接口。
  // connect(2) 需要 Unix Socket 写权限，不能依赖 root 的 DAC capability
  //（加固 unit 已清空 CapabilityBoundingSet）。
  if (::chmod(socket_path_.c_str(), 0660) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    ::unlink(socket_path_.c_str());
    return false;
  }

  if (::listen(listen_fd_, 8) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  running_.store(true);
  server_thread_ = std::thread(&VideoHealthServer::server_loop, this);
  return true;
}

void VideoHealthServer::stop() {
  bool was = running_.exchange(false);
  if (!was) return;
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (server_thread_.joinable()) server_thread_.join();
  if (!socket_path_.empty()) {
    ::unlink(socket_path_.c_str());
  }
}

void VideoHealthServer::update_state(const VideoHealthState& state) {
  std::lock_guard<std::mutex> lk(state_mutex_);
  state_ = state;
}

void VideoHealthServer::server_loop() {
  while (running_.load()) {
    int client_fd = ::accept(listen_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      if (!running_.load()) break;
      continue;
    }

    // 设置超时
    struct timeval tv{3, 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // 读取请求
    char buf[4096];
    std::string req;
    ssize_t n;
    while ((n = ::recv(client_fd, buf, sizeof(buf), 0)) > 0) {
      req.append(buf, n);
      if (req.find('\n') != std::string::npos) break;
      if (req.size() > 4096) break;
    }

    // 解析 action
    std::string action = "health";
    // 简单提取 "action":"xxx"
    auto pos = req.find("\"action\"");
    if (pos != std::string::npos) {
      auto colon = req.find(':', pos);
      if (colon != std::string::npos) {
        auto q1 = req.find('"', colon + 1);
        if (q1 != std::string::npos) {
          auto q2 = req.find('"', q1 + 1);
          if (q2 != std::string::npos) {
            action = req.substr(q1 + 1, q2 - q1 - 1);
          }
        }
      }
    }

    std::string resp = handle_request(action) + "\n";
    ::send(client_fd, resp.data(), resp.size(), MSG_NOSIGNAL);
    ::close(client_fd);
  }
}

std::string VideoHealthServer::handle_request(const std::string& action) {
  if (action == "metrics") return build_metrics_json();
  if (action == "version") return build_version_json();
  return build_health_json();
}

std::string VideoHealthServer::build_health_json() {
  read_proc_info();
  std::lock_guard<std::mutex> lk(state_mutex_);
  std::ostringstream oss;
  oss << "{";
  oss << "\"available\":true";
  oss << ",\"status\":\"" << escape_json(state_.status) << "\"";
  oss << ",\"release\":\"" << escape_json(state_.release) << "\"";
  oss << ",\"version\":\"" << escape_json(state_.version) << "\"";
  oss << ",\"commit\":\"" << escape_json(state_.commit) << "\"";
  oss << ",\"business_state\":\"" << escape_json(state_.business_state) << "\"";
  oss << ",\"business_link\":\"" << escape_json(state_.business_link) << "\"";
  oss << ",\"enrichment_mode\":\"" << escape_json(state_.enrichment_mode) << "\"";
  oss << ",\"business_timeout_count\":" << state_.business_timeout_count;
  oss << ",\"business_error_count\":" << state_.business_error_count;
  oss << ",\"degradation\":\"" << escape_json(state_.degradation) << "\"";
  oss << ",\"health_reason\":\"" << escape_json(state_.health_reason) << "\"";
  oss << ",\"uptime_seconds\":" << state_.uptime_seconds;
  oss << ",\"rss_mb\":" << state_.rss_mb;
  oss << ",\"tpu_info\":\"" << escape_json(state_.tpu_info) << "\"";
  oss << ",\"resource\":{"
      << "\"rss_mb\":" << state_.rss_mb
      << ",\"tpu_info\":\"" << escape_json(state_.tpu_info) << "\""
      << ",\"fatal\":" << (state_.resource_fatal ? "true" : "false") << "}";
  oss << ",\"storage\":{"
      << "\"path\":\"" << escape_json(state_.storage.path) << "\""
      << ",\"available\":" << (state_.storage.available ? "true" : "false")
      << ",\"free_bytes\":" << state_.storage.free_bytes
      << ",\"state\":\"" << escape_json(state_.storage.state) << "\"}";
  oss << ",\"event_writer\":{"
      << event_writer_to_json("A", state_.event_writer_a) << ","
      << event_writer_to_json("B", state_.event_writer_b) << "}";
  oss << ",\"active_alerts\":[";
  for (size_t i = 0; i < state_.active_alerts.size(); ++i) {
    if (i) oss << ",";
    oss << "\"" << escape_json(state_.active_alerts[i]) << "\"";
  }
  oss << "]";
  oss << ",\"streams\":{";
  oss << stream_to_json("A", state_.stream_a);
  oss << "," << stream_to_json("B", state_.stream_b);
  oss << "}}";
  return oss.str();
}

std::string VideoHealthServer::build_metrics_json() {
  return build_health_json();
}

std::string VideoHealthServer::build_version_json() {
  std::lock_guard<std::mutex> lk(state_mutex_);
  std::ostringstream oss;
  oss << "{";
  oss << "\"release\":\"" << escape_json(state_.release) << "\"";
  oss << ",\"version\":\"" << escape_json(state_.version) << "\"";
  oss << ",\"commit\":\"" << escape_json(state_.commit) << "\"";
  oss << "}";
  return oss.str();
}

void VideoHealthServer::read_proc_info() {
  // RSS from /proc/self/status
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line)) {
    if (line.compare(0, 6, "VmRSS:") == 0) {
      long kb = 0;
      std::sscanf(line.c_str(), "VmRSS: %ld", &kb);
      std::lock_guard<std::mutex> lk(state_mutex_);
      state_.rss_mb = kb / 1024.0;
      break;
    }
  }

  // TPU info (best effort, non-blocking)
  std::lock_guard<std::mutex> lk(state_mutex_);
  // 尝试读取 TPU 内存
  std::ifstream tf("/sys/kernel/debug/bm1684/memory_usage");
  if (tf) {
    std::string content((std::istreambuf_iterator<char>(tf)),
                         std::istreambuf_iterator<char>());
    if (!content.empty()) {
      state_.tpu_info = content.substr(0, 200);
      return;
    }
  }
  state_.tpu_info = "unknown";
}

}  // namespace hzw
