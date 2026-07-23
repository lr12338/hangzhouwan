// -*- coding: utf-8 -*-
#include "monitoring/video_health_logic.h"

namespace hzw {

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
  // 资源致命：VPU/gmem 分配失败或解码器/编码器初始化失败 -> FAILED
  if (s.resource_fatal) return StreamHealthLevel::FAILED;
  // RTSP 断开（最近帧超阈值未更新，调用方据此设 rtsp_connected=false）-> FAILED
  if (!s.rtsp_connected) return StreamHealthLevel::FAILED;
  // 已连接但输出持续不足 -> DEGRADED
  if (s.output_fps < t.min_output_fps) return StreamHealthLevel::DEGRADED;
  // RTMP 断开（有解码输入但无 RTMP 输出）-> DEGRADED
  if (!s.rtmp_connected) return StreamHealthLevel::DEGRADED;
  // 推理持续为 0（已连接却无推理）-> DEGRADED
  if (s.inference_fps <= 0.0) return StreamHealthLevel::DEGRADED;
  // 重连风暴（短时反复重连）-> DEGRADED
  if (s.reconnect_delta > t.reconnect_storm_delta) return StreamHealthLevel::DEGRADED;
  return StreamHealthLevel::HEALTHY;
}

DualHealthResult compute_dual_health(const StreamHealthInput& a,
                                     const StreamHealthInput& b,
                                     bool business_full,
                                     const HealthThresholds& t) {
  DualHealthResult r;
  r.level_a = compute_stream_level(a, t);
  r.level_b = compute_stream_level(b, t);

  const bool any_fatal = a.resource_fatal || b.resource_fatal;
  const bool a_failed = (r.level_a == StreamHealthLevel::FAILED);
  const bool b_failed = (r.level_b == StreamHealthLevel::FAILED);
  const bool a_degraded = (r.level_a == StreamHealthLevel::DEGRADED);
  const bool b_degraded = (r.level_b == StreamHealthLevel::DEGRADED);

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
