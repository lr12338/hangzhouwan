// -*- coding: utf-8 -*-
// 视频健康判定单元测试（纯逻辑，不依赖网络/硬件/FFmpeg）。
// 验证门禁 6.5：RTSP断开/RTMP断开/FPS归零/资源致命/重连风暴 -> DEGRADED/FAILED，
// 并覆盖 A/B 隔离（B 路故障不影响 A 路健康判定）。
#include <cstdio>
#include <iostream>
#include "monitoring/video_health_logic.h"

using namespace hzw;

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "失败 | " << #cond << " @行" << __LINE__ << "\n";         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

int main() {
  HealthThresholds t;
  StreamHealthInput healthy;
  healthy.rtsp_connected = true;
  healthy.rtmp_connected = true;
  healthy.output_fps = 10.0;
  healthy.inference_fps = 5.0;

  // ---- 1) 资源致命 -> FAILED ----
  {
    StreamHealthInput s = healthy;
    s.resource_fatal = true;
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::FAILED);
    auto r = compute_dual_health(s, healthy, true, t);
    CHECK(r.status == "FAILED");
    CHECK(r.degradation == "resource_fatal");
    CHECK(r.level_a == StreamHealthLevel::FAILED);
    CHECK(r.level_b == StreamHealthLevel::HEALTHY);
  }

  // ---- 2) RTSP 断开 -> FAILED ----
  {
    StreamHealthInput s = healthy;
    s.rtsp_connected = false;
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::FAILED);
    auto r = compute_dual_health(healthy, s, true, t);
    CHECK(r.status == "DEGRADED");
    CHECK(r.degradation == "stream_down");
    CHECK(r.level_a == StreamHealthLevel::HEALTHY);
    CHECK(r.level_b == StreamHealthLevel::FAILED);
  }

  // ---- 3) 双路均断开 -> FAILED ----
  {
    StreamHealthInput s = healthy;
    s.rtsp_connected = false;
    auto r = compute_dual_health(s, s, true, t);
    CHECK(r.status == "FAILED");
    CHECK(r.degradation == "stream_down");
  }

  // ---- 4) output_fps 低于阈值 -> DEGRADED ----
  {
    StreamHealthInput s = healthy;
    s.output_fps = 2.0;
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::DEGRADED);
    auto r = compute_dual_health(s, healthy, true, t);
    CHECK(r.status == "DEGRADED");
    CHECK(r.degradation == "stream_degraded");
  }

  // ---- 5) RTMP 断开（有输入无输出）-> DEGRADED ----
  {
    StreamHealthInput s = healthy;
    s.rtmp_connected = false;
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::DEGRADED);
  }

  // ---- 6) inference_fps 持续为 0 -> DEGRADED ----
  {
    StreamHealthInput s = healthy;
    s.inference_fps = 0.0;
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::DEGRADED);
  }

  // ---- 7) 重连风暴 -> DEGRADED ----
  {
    StreamHealthInput s = healthy;
    s.reconnect_delta = 5;  // > 阈值 3
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::DEGRADED);
    s.reconnect_delta = 3;  // 等于阈值，不触发
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::HEALTHY);
  }

  // ---- 8) 全健康 -> HEALTHY ----
  {
    CHECK(compute_stream_level(healthy, t) == StreamHealthLevel::HEALTHY);
    auto r = compute_dual_health(healthy, healthy, true, t);
    CHECK(r.status == "HEALTHY");
    CHECK(r.degradation == "none");
  }

  // ---- 9) 业务仅检测（business_full=false）-> DEGRADED detection_only ----
  {
    auto r = compute_dual_health(healthy, healthy, false, t);
    CHECK(r.status == "DEGRADED");
    CHECK(r.degradation == "detection_only");
  }

  // ---- 10) A/B 隔离：B 路资源致命不影响 A 路等级 ----
  {
    StreamHealthInput b = healthy;
    b.resource_fatal = true;
    auto r = compute_dual_health(healthy, b, true, t);
    CHECK(r.level_a == StreamHealthLevel::HEALTHY);
    CHECK(r.level_b == StreamHealthLevel::FAILED);
    CHECK(r.status == "FAILED");
  }

  // ---- 11) 资源致命优先于双路断开（最高严重度）----
  {
    StreamHealthInput a = healthy;
    a.resource_fatal = true;
    StreamHealthInput b = healthy;
    b.rtsp_connected = false;
    auto r = compute_dual_health(a, b, true, t);
    CHECK(r.status == "FAILED");
    CHECK(r.degradation == "resource_fatal");
  }

  if (g_failures == 0) std::cout << "通过 | 视频健康判定单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
