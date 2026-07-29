// -*- coding: utf-8 -*-
#include "monitoring/video_health_logic.h"

#include <algorithm>

namespace hzw {

namespace {
// 严重度排序：HEALTHY(0) < DEGRADED(1) < FAILED(2)。
int severity(StreamHealthLevel l) {
  switch (l) {
    case StreamHealthLevel::STARTING: return 0;
    case StreamHealthLevel::HEALTHY: return 0;
    case StreamHealthLevel::DEGRADED: return 1;
    case StreamHealthLevel::FAILED: return 2;
  }
  return 2;
}
}  // namespace

const char* level_str(StreamHealthLevel l) {
  switch (l) {
    case StreamHealthLevel::STARTING: return "STARTING";
    case StreamHealthLevel::HEALTHY: return "HEALTHY";
    case StreamHealthLevel::DEGRADED: return "DEGRADED";
    case StreamHealthLevel::FAILED: return "FAILED";
  }
  return "UNKNOWN";
}

BusinessHealthResult compute_business_health(
    bool enabled_a, bool link_a, const std::string& mode_a,
    bool enabled_b, bool link_b, const std::string& mode_b) {
  BusinessHealthResult result;
  const bool enabled = enabled_a || enabled_b;
  if (!enabled) {
    result.link = "DISABLED";
    result.mode = "PENDING";
    result.link_healthy = true;
    return result;
  }
  result.link_healthy = (!enabled_a || link_a) && (!enabled_b || link_b);
  result.link = result.link_healthy ? "HEALTHY" : "FAILED";
  if (mode_a == "FULL" || mode_b == "FULL") {
    result.mode = "FULL";
  } else if (mode_a == "COORD_ONLY" || mode_b == "COORD_ONLY") {
    result.mode = "COORD_ONLY";
  } else if (result.link_healthy) {
    result.mode = "PENDING";
  } else {
    result.mode = "DETECTION_ONLY";
  }
  return result;
}

int64_t update_reconnect_attempt_window(
    std::deque<int64_t>& timestamps_ms, int64_t total_attempts,
    int64_t& previous_total, int64_t now_ms, int64_t window_ms) {
  const int64_t delta = std::max<int64_t>(
      0, total_attempts - previous_total);
  previous_total = total_attempts;
  for (int64_t i = 0; i < delta; ++i) {
    timestamps_ms.push_back(now_ms);
  }
  while (!timestamps_ms.empty() &&
         timestamps_ms.front() <= now_ms - window_ms) {
    timestamps_ms.pop_front();
  }
  return static_cast<int64_t>(timestamps_ms.size());
}

std::string compute_event_writer_state(
    bool required_and_pipeline_running, bool writer_running,
    bool write_enabled, bool has_error) {
  if (!writer_running) {
    return (required_and_pipeline_running || has_error)
               ? "FAILED" : "NOT_STARTED";
  }
  return write_enabled ? "RUNNING" : "PROTECTED";
}

std::string compute_storage_state(
    bool data_available, int64_t free_bytes,
    int64_t warning_bytes, int64_t protected_bytes) {
  if (!data_available) return "UNAVAILABLE";
  if (free_bytes < protected_bytes) return "PROTECTED";
  if (free_bytes < warning_bytes) return "WARNING";
  return "OK";
}

StreamHealthLevel compute_stream_level(const StreamHealthInput& s,
                                       const HealthThresholds& t) {
  if (s.thread_failed) return StreamHealthLevel::FAILED;
  // 资源致命：VPU/gmem 分配失败或解码器/编码器初始化失败 -> FAILED（最高优先级）
  if (s.resource_fatal) return StreamHealthLevel::FAILED;
  if (s.starting) return StreamHealthLevel::STARTING;
  // RTSP 断开分级：持续断流(>=grace) -> FAILED；瞬时断流/重连中(stale~grace) -> DEGRADED
  if (s.disconnected_ms >= t.reconnect_grace_ms) return StreamHealthLevel::FAILED;
  if (s.disconnected_ms >= t.frame_stale_ms) return StreamHealthLevel::DEGRADED;
  // 已连接（最近帧在 stale 阈值内）
  if (s.output_fps < t.min_output_fps) return StreamHealthLevel::DEGRADED;
  if (!s.rtmp_connected) return StreamHealthLevel::DEGRADED;
  if (s.inference_fps < t.min_inference_fps) return StreamHealthLevel::DEGRADED;
  if (s.reconnects_1h > t.max_reconnects_per_hour) return StreamHealthLevel::DEGRADED;
  return StreamHealthLevel::HEALTHY;
}

StreamHealthLevel StreamHealthDebouncer::update(StreamHealthLevel instantaneous,
                                                int confirm_down, int confirm_up) {
  if (confirm_down < 1) confirm_down = 1;
  if (confirm_up < 1) confirm_up = 1;
  // 瞬时 FAILED 立即提交：关键错误/持续断流不应被防抖延迟告警。
  if (instantaneous == StreamHealthLevel::FAILED) {
    committed_ = StreamHealthLevel::FAILED;
    candidate_ = StreamHealthLevel::FAILED;
    streak_ = 0;
    return committed_;
  }
  if (instantaneous == StreamHealthLevel::STARTING) {
    committed_ = StreamHealthLevel::STARTING;
    candidate_ = StreamHealthLevel::STARTING;
    streak_ = 0;
    return committed_;
  }
  if (committed_ == StreamHealthLevel::STARTING) {
    // 启动宽限结束后的首个结论立即生效，不能再叠加普通防抖窗口。
    committed_ = instantaneous;
    candidate_ = instantaneous;
    streak_ = 0;
    return committed_;
  }
  // 已处于该等级：重置候选，保持稳定。
  if (instantaneous == committed_) {
    candidate_ = instantaneous;
    streak_ = 0;
    return committed_;
  }
  // 状态变化：累计连续确认次数。恶化用 confirm_down，恢复用 confirm_up。
  if (instantaneous != candidate_) {
    candidate_ = instantaneous;
    streak_ = 1;
  } else {
    ++streak_;
  }
  int need = (severity(instantaneous) > severity(committed_)) ? confirm_down : confirm_up;
  if (streak_ >= need) {
    committed_ = candidate_;
    streak_ = 0;
  }
  return committed_;
}

DualHealthResult compute_dual_status(StreamHealthLevel level_a,
                                     StreamHealthLevel level_b,
                                     bool fatal_a, bool fatal_b,
                                     bool business_link_healthy,
                                     bool enabled_a,
                                     bool enabled_b) {
  DualHealthResult r;
  r.level_a = level_a;
  r.level_b = level_b;

  const bool any_fatal = (enabled_a && fatal_a) || (enabled_b && fatal_b);
  const bool a_failed = enabled_a && (level_a == StreamHealthLevel::FAILED);
  const bool b_failed = enabled_b && (level_b == StreamHealthLevel::FAILED);
  const bool a_degraded =
      enabled_a && (level_a == StreamHealthLevel::DEGRADED);
  const bool b_degraded =
      enabled_b && (level_b == StreamHealthLevel::DEGRADED);
  const bool any_starting =
      (enabled_a && level_a == StreamHealthLevel::STARTING) ||
      (enabled_b && level_b == StreamHealthLevel::STARTING);

  if (any_fatal) {
    r.status = "FAILED";
    r.degradation = "resource_fatal";
    r.reason = "设备资源致命(VPU/gmem)";
  } else if (a_failed || b_failed) {
    r.status = "FAILED";
    r.degradation = "stream_down";
    if (a_failed && b_failed) r.reason = "A/B 双路均不可用";
    else r.reason = a_failed ? "A路不可用" : "B路不可用";
  } else if (a_degraded || b_degraded) {
    r.status = "DEGRADED";
    r.degradation = "stream_degraded";
    r.reason = std::string(a_degraded ? "A" : "") + std::string(b_degraded ? "B" : "") + "路降级";
  } else if (!business_link_healthy) {
    r.status = "DEGRADED";
    r.degradation = "detection_only";
    r.reason = "Sidecar链路不可用(仅检测)";
  } else if (any_starting) {
    r.status = "STARTING";
    r.degradation = "starting";
    r.reason = "视频流启动中";
  } else {
    r.status = "HEALTHY";
    r.degradation = "none";
    r.reason = "";
  }
  return r;
}

}  // namespace hzw
