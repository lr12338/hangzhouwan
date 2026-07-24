// -*- coding: utf-8 -*-
#include "monitoring/video_health_logic.h"

namespace hzw {

namespace {
// 严重度排序：HEALTHY(0) < DEGRADED(1) < FAILED(2)。
int severity(StreamHealthLevel l) {
  switch (l) {
    case StreamHealthLevel::HEALTHY: return 0;
    case StreamHealthLevel::DEGRADED: return 1;
    case StreamHealthLevel::FAILED: return 2;
  }
  return 2;
}
}  // namespace

const char* level_str(StreamHealthLevel l) {
  switch (l) {
    case StreamHealthLevel::HEALTHY: return "HEALTHY";
    case StreamHealthLevel::DEGRADED: return "DEGRADED";
    case StreamHealthLevel::FAILED: return "FAILED";
  }
  return "UNKNOWN";
}

StreamHealthLevel compute_stream_level(const StreamHealthInput& s,
                                       const HealthThresholds& t) {
  // 资源致命：VPU/gmem 分配失败或解码器/编码器初始化失败 -> FAILED（最高优先级）
  if (s.resource_fatal) return StreamHealthLevel::FAILED;
  // RTSP 断开分级：持续断流(>=grace) -> FAILED；瞬时断流/重连中(stale~grace) -> DEGRADED
  if (s.disconnected_ms >= t.reconnect_grace_ms) return StreamHealthLevel::FAILED;
  if (s.disconnected_ms >= t.frame_stale_ms) return StreamHealthLevel::DEGRADED;
  // 已连接（最近帧在 stale 阈值内）
  if (s.output_fps < t.min_output_fps) return StreamHealthLevel::DEGRADED;
  if (!s.rtmp_connected) return StreamHealthLevel::DEGRADED;
  if (s.inference_fps <= 0.0) return StreamHealthLevel::DEGRADED;
  if (s.reconnect_delta > t.reconnect_storm_delta) return StreamHealthLevel::DEGRADED;
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
                                     bool business_full) {
  DualHealthResult r;
  r.level_a = level_a;
  r.level_b = level_b;

  const bool any_fatal = fatal_a || fatal_b;
  const bool a_failed = (level_a == StreamHealthLevel::FAILED);
  const bool b_failed = (level_b == StreamHealthLevel::FAILED);
  const bool a_degraded = (level_a == StreamHealthLevel::DEGRADED);
  const bool b_degraded = (level_b == StreamHealthLevel::DEGRADED);

  if (any_fatal) {
    r.status = "FAILED";
    r.degradation = "resource_fatal";
    r.reason = "设备资源致命(VPU/gmem)";
  } else if (a_failed && b_failed) {
    r.status = "FAILED";
    r.degradation = "stream_down";
    r.reason = "A/B 双路均不可用";
  } else if (a_failed) {
    r.status = "DEGRADED";
    r.degradation = "stream_down";
    r.reason = "A路不可用";
  } else if (b_failed) {
    r.status = "DEGRADED";
    r.degradation = "stream_down";
    r.reason = "B路不可用";
  } else if (a_degraded || b_degraded) {
    r.status = "DEGRADED";
    r.degradation = "stream_degraded";
    r.reason = std::string(a_degraded ? "A" : "") + std::string(b_degraded ? "B" : "") + "路降级";
  } else if (!business_full) {
    r.status = "DEGRADED";
    r.degradation = "detection_only";
    r.reason = "业务增强降级(仅检测)";
  } else {
    r.status = "HEALTHY";
    r.degradation = "none";
    r.reason = "";
  }
  return r;
}

}  // namespace hzw
