// -*- coding: utf-8 -*-
// 视频健康判定单元测试（纯逻辑，不依赖网络/硬件/FFmpeg）。
// 覆盖门禁 6.5 全部场景 + 防抖：瞬时断流/重连宽限/持续断流/单双路失败/VPU致命/
// FPS低/inference0/RTMP断/重连风暴/恢复防抖/A-B隔离/致命优先级。
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
  StreamHealthInput connected;
  connected.disconnected_ms = 0;
  connected.rtmp_connected = true;
  connected.output_fps = 10.0;
  connected.inference_fps = 5.0;

  // ---- 1) 资源致命 -> FAILED（最高优先级）----
  {
    StreamHealthInput s = connected;
    s.resource_fatal = true;
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::FAILED);
    auto r = compute_dual_status(StreamHealthLevel::FAILED, StreamHealthLevel::HEALTHY,
                                 true, false, true);
    CHECK(r.status == "FAILED");
    CHECK(r.degradation == "resource_fatal");
  }

  // ---- 2) 瞬时 RTSP 断流（stale~grace，重连宽限）-> DEGRADED，不立即 FAILED ----
  {
    StreamHealthInput s = connected;
    s.disconnected_ms = t.frame_stale_ms + 1000;  // 16s，超过 stale 但在 grace 内
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::DEGRADED);
  }

  // ---- 3) 持续 RTSP 断流（>=grace）-> FAILED ----
  {
    StreamHealthInput s = connected;
    s.disconnected_ms = t.reconnect_grace_ms;  // 60s，恰好超宽限
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::FAILED);
  }

  // ---- 4) output_fps 低于阈值 -> DEGRADED ----
  {
    StreamHealthInput s = connected;
    s.output_fps = 2.0;
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::DEGRADED);
  }

  // ---- 5) RTMP 断开（有输入无输出）-> DEGRADED ----
  {
    StreamHealthInput s = connected;
    s.rtmp_connected = false;
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::DEGRADED);
  }

  // ---- 6) inference_fps 持续为 0 -> DEGRADED ----
  {
    StreamHealthInput s = connected;
    s.inference_fps = 0.0;
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::DEGRADED);
  }

  // ---- 7) 重连风暴 -> DEGRADED（>阈值触发，==阈值不触发）----
  {
    StreamHealthInput s = connected;
    s.reconnect_delta = 5;
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::DEGRADED);
    s.reconnect_delta = 3;
    CHECK(compute_stream_level(s, t) == StreamHealthLevel::HEALTHY);
  }

  // ---- 8) 全健康 -> HEALTHY ----
  {
    CHECK(compute_stream_level(connected, t) == StreamHealthLevel::HEALTHY);
  }

  // ---- 9) 单路 FAILED、另一路正常 -> 整体 DEGRADED，突出故障流 ----
  {
    auto r = compute_dual_status(StreamHealthLevel::HEALTHY, StreamHealthLevel::FAILED,
                                 false, false, true);
    CHECK(r.status == "DEGRADED");
    CHECK(r.degradation == "stream_down");
    CHECK(r.level_a == StreamHealthLevel::HEALTHY);
    CHECK(r.level_b == StreamHealthLevel::FAILED);
  }

  // ---- 10) 双路 FAILED -> 整体 FAILED ----
  {
    auto r = compute_dual_status(StreamHealthLevel::FAILED, StreamHealthLevel::FAILED,
                                 false, false, true);
    CHECK(r.status == "FAILED");
    CHECK(r.degradation == "stream_down");
  }

  // ---- 11) A/B 隔离 + 资源致命优先：B 致命不影响 A 等级；致命优先于双路断 ----
  {
    auto r = compute_dual_status(StreamHealthLevel::HEALTHY, StreamHealthLevel::FAILED,
                                 false, true, true);
    CHECK(r.level_a == StreamHealthLevel::HEALTHY);
    CHECK(r.status == "FAILED");
    CHECK(r.degradation == "resource_fatal");
  }

  // ---- 12) 业务仅检测 -> DEGRADED detection_only ----
  {
    auto r = compute_dual_status(StreamHealthLevel::HEALTHY, StreamHealthLevel::HEALTHY,
                                 false, false, false);
    CHECK(r.status == "DEGRADED");
    CHECK(r.degradation == "detection_only");
  }

  // ---- 13) 防抖：短暂断流(瞬时 DEGRADED)不立即整体 FAILED；需连续确认才降级 ----
  {
    StreamHealthDebouncer db;
    // 启动健康，单次瞬时 DEGRADED（毛刺）不应提交
    CHECK(db.update(StreamHealthLevel::DEGRADED, t.confirm_down, t.confirm_up) == StreamHealthLevel::HEALTHY);
    // 第二次连续 DEGRADED 才提交（confirm_down=2）
    CHECK(db.update(StreamHealthLevel::DEGRADED, t.confirm_down, t.confirm_up) == StreamHealthLevel::DEGRADED);
  }

  // ---- 14) 防抖：FAILED 立即提交（不过度延迟）----
  {
    StreamHealthDebouncer db;
    CHECK(db.update(StreamHealthLevel::FAILED, t.confirm_down, t.confirm_up) == StreamHealthLevel::FAILED);
    CHECK(db.committed() == StreamHealthLevel::FAILED);
  }

  // ---- 15) 防抖：恢复需连续确认，避免假恢复抖动 ----
  {
    StreamHealthDebouncer db;
    db.update(StreamHealthLevel::FAILED, t.confirm_down, t.confirm_up);  // -> FAILED
    // 单次 HEALTHY 不恢复（confirm_up=3）
    CHECK(db.update(StreamHealthLevel::HEALTHY, t.confirm_down, t.confirm_up) == StreamHealthLevel::FAILED);
    CHECK(db.update(StreamHealthLevel::HEALTHY, t.confirm_down, t.confirm_up) == StreamHealthLevel::FAILED);
    // 第三次连续 HEALTHY 才恢复
    CHECK(db.update(StreamHealthLevel::HEALTHY, t.confirm_down, t.confirm_up) == StreamHealthLevel::HEALTHY);
  }

  // ---- 16) 防抖：恢复后健康状态可回到 HEALTHY ----
  {
    StreamHealthDebouncer db;
    db.update(StreamHealthLevel::DEGRADED, t.confirm_down, t.confirm_up);
    db.update(StreamHealthLevel::DEGRADED, t.confirm_down, t.confirm_up);  // -> DEGRADED
    for (int i = 0; i < t.confirm_up; ++i)
      db.update(StreamHealthLevel::HEALTHY, t.confirm_down, t.confirm_up);
    CHECK(db.committed() == StreamHealthLevel::HEALTHY);
  }

  // ---- 17) systemd active 但数据面失败绝不 HEALTHY（瞬时 FAILED 立即提交）----
  {
    StreamHealthDebouncer db;
    db.update(StreamHealthLevel::HEALTHY, t.confirm_down, t.confirm_up);
    db.update(StreamHealthLevel::FAILED, t.confirm_down, t.confirm_up);  // 资源致命/持续断流
    auto r = compute_dual_status(db.committed(), StreamHealthLevel::HEALTHY,
                                 true, false, true);
    CHECK(r.status == "FAILED");
    CHECK(r.status != "HEALTHY");
  }

  if (g_failures == 0) std::cout << "通过 | 视频健康判定单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
