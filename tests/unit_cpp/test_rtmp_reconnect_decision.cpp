// -*- coding: utf-8 -*-
// RTMP 重连决策单元测试（纯逻辑，不依赖网络/硬件）。
// 验证：普通网络失败仅重建 muxer/AVIO（保留编码器）；
//       编码器 ENOMEM 或编码器未打开 -> ESCALATE_FATAL。
#include <cstdio>
#include <iostream>
#include "video/ffmpeg_compat.h"
#include "video/video_sink.h"

using hzw::decide_rtmp_reconnect;
using hzw::RtmpReconnectAction;
using hzw::should_log_rtmp_reconnect;

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "失败 | " << #cond << " @行" << __LINE__ << "\n";         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

int main() {
  // ---- 1) 普通网络写失败：仅重建 muxer ----
  {
    // AVERROR(EPIPE) 等普通网络错误 -> 保留编码器
    CHECK(decide_rtmp_reconnect(AVERROR(EPIPE), true) == RtmpReconnectAction::REBUILD_MUXER_ONLY);
    CHECK(decide_rtmp_reconnect(AVERROR(EIO), true) == RtmpReconnectAction::REBUILD_MUXER_ONLY);
    CHECK(decide_rtmp_reconnect(AVERROR(ECONNRESET), true) == RtmpReconnectAction::REBUILD_MUXER_ONLY);
    // 写帧返回 0（成功）不触发重连决策，但仍应判定为非致命
    CHECK(decide_rtmp_reconnect(0, true) == RtmpReconnectAction::REBUILD_MUXER_ONLY);
  }

  // ---- 2) 编码器 ENOMEM：升级致命 ----
  {
    CHECK(decide_rtmp_reconnect(AVERROR(ENOMEM), true) == RtmpReconnectAction::ESCALATE_FATAL);
  }

  // ---- 3) 编码器未打开：升级致命 ----
  {
    CHECK(decide_rtmp_reconnect(AVERROR(EPIPE), false) == RtmpReconnectAction::ESCALATE_FATAL);
    CHECK(decide_rtmp_reconnect(0, false) == RtmpReconnectAction::ESCALATE_FATAL);
    CHECK(decide_rtmp_reconnect(AVERROR(ENOMEM), false) == RtmpReconnectAction::ESCALATE_FATAL);
  }

  // ---- 4) 与旧实现（每次重建编码器）的行为差异：ENOMEM 必须致命而非重建 ----
  {
    // 关键回归：旧实现 RTMP 重连无条件重建编码器；新实现 ENOMEM 立即熔断。
    auto a = decide_rtmp_reconnect(AVERROR(ENOMEM), true);
    CHECK(a == RtmpReconnectAction::ESCALATE_FATAL);
  }

  // ---- 5) RTMP 重连日志节流：首次及每 10 次输出，其余静默 ----
  {
    constexpr int INTERVAL = 10;
    // 首次（count=0）总是输出
    CHECK(should_log_rtmp_reconnect(0, INTERVAL) == true);
    // 1-9 静默
    for (int i = 1; i < 10; ++i) {
      CHECK(should_log_rtmp_reconnect(i, INTERVAL) == false);
    }
    // 10 输出
    CHECK(should_log_rtmp_reconnect(10, INTERVAL) == true);
    // 11-19 静默
    for (int i = 11; i < 20; ++i) {
      CHECK(should_log_rtmp_reconnect(i, INTERVAL) == false);
    }
    // 20 输出
    CHECK(should_log_rtmp_reconnect(20, INTERVAL) == true);
    // 100 输出
    CHECK(should_log_rtmp_reconnect(100, INTERVAL) == true);
    // 105 静默
    CHECK(should_log_rtmp_reconnect(105, INTERVAL) == false);
    // 验证 1000 次重连日志量：count 0..999，输出条件 count==0||count%10==0
    // -> 0,10,20,...,990 共 100 条，日志量降至 10%（vs 不节流 1000 条）
    int log_count = 0;
    for (int i = 0; i < 1000; ++i) {
      if (should_log_rtmp_reconnect(i, INTERVAL)) ++log_count;
    }
    CHECK(log_count == 100);
  }

  if (g_failures == 0) std::cout << "通过 | RTMP 重连决策单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
