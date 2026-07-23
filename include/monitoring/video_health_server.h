// -*- coding: utf-8 -*-
// =============================================================================
// VideoHealthServer：Video 服务结构化健康接口（Unix Socket）。
//
// 监听 /run/hangzhouwan/video-health.sock，支持 JSON 请求：
//   {"action": "health"}   - 健康摘要
//   {"action": "metrics"}  - 详细指标
//   {"action": "version"}  - 版本信息
//
// 返回结构化 JSON，包含：
//   release/version/commit、A/B RTSP/RTMP 状态、output/inference fps、
//   最近帧时间、RTSP/RTMP 重连次数、队列长度、e2e P95、
//   Business 连接状态、降级状态、RSS 和 TPU 信息。
//
// hzwctl status/health 优先读取此接口，journal 只作为诊断补充。
// =============================================================================
#ifndef HZW_MONITORING_VIDEO_HEALTH_SERVER_H
#define HZW_MONITORING_VIDEO_HEALTH_SERVER_H

#include <atomic>
#include <cstdint>
#include <string>
#include <mutex>
#include <thread>

namespace hzw {

// 单路流健康快照（由外部定期更新）
struct StreamHealthSnapshot {
  std::string stream_id;
  std::string level = "HEALTHY";  // 单路 HEALTHY|DEGRADED|FAILED
  bool rtsp_connected = false;
  bool rtmp_connected = false;
  double output_fps = 0.0;
  double inference_fps = 0.0;
  int64_t last_frame_time_ms = 0;     // 墙钟时间
  int64_t rtsp_reconnects = 0;
  int64_t rtmp_reconnects = 0;
  int queue_length = 0;
  double e2e_p95_ms = 0.0;
};

// 整体健康状态（由外部定期更新）
struct VideoHealthState {
  std::string release;
  std::string version;
  std::string commit;
  std::string status = "UNKNOWN";       // HEALTHY | DEGRADED | FAILED
  std::string business_state = "UNKNOWN"; // FULL | COORD_ONLY | DETECTION_ONLY
  std::string degradation = "none";     // none | stream_degraded | stream_down | detection_only | resource_fatal
  std::string health_reason;            // 降级/失败原因（人类可读）
  StreamHealthSnapshot stream_a;
  StreamHealthSnapshot stream_b;
  double rss_mb = 0.0;
  std::string tpu_info = "unknown";
  int64_t uptime_seconds = 0;
};

class VideoHealthServer {
 public:
  VideoHealthServer();
  ~VideoHealthServer();

  // 启动健康 Socket 服务。失败返回 false。
  bool start(const std::string& socket_path);

  // 停止服务并清理 Socket 文件。
  void stop();

  // 更新整体状态（线程安全，由 metrics_loop 调用）。
  void update_state(const VideoHealthState& state);

 private:
  void server_loop();
  std::string handle_request(const std::string& action);
  std::string build_health_json();
  std::string build_metrics_json();
  std::string build_version_json();
  void read_proc_info();

  int listen_fd_ = -1;
  std::string socket_path_;
  std::thread server_thread_;
  std::atomic<bool> running_{false};

  // 当前状态（mutex 保护）
  VideoHealthState state_;
  mutable std::mutex state_mutex_;
};

}  // namespace hzw

#endif  // HZW_MONITORING_VIDEO_HEALTH_SERVER_H
