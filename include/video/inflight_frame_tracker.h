// -*- coding: utf-8 -*-
// =============================================================================
// InflightFrameTracker：在途 AVFrame 引用计数器（纯逻辑，不依赖 FFmpeg/硬件）。
//
// 用途：SophonVideoSource 每产生一帧（avcodec_receive_frame 成功）调用 on_produce()，
//       帧被管线最终 release() 时调用 on_release()。RTSP 重连关闭旧解码器前必须等待
//       count() 归零——否则旧解码器 bm_image 池中仍被在途帧引用的设备内存会泄漏
//       （avcodec_free_context 无法回收仍被外部 AVFrame 引用的 bm_image）。
//
// 线程安全：原子计数，可被解码线程/处理线程/编码线程并发访问。
// 可独立单元测试：不依赖网络、硬件、FFmpeg，仅验证计数与超时语义。
// =============================================================================
#ifndef HZW_VIDEO_INFLIGHT_FRAME_TRACKER_H
#define HZW_VIDEO_INFLIGHT_FRAME_TRACKER_H

#include <atomic>
#include <chrono>
#include <thread>

namespace hzw {

class InflightFrameTracker {
 public:
  // 产生一帧：计数 +1。
  void on_produce() { count_.fetch_add(1, std::memory_order_acq_rel); }

  // 释放一帧：计数 -1（绝不低于 0；调用方保证 produce/release 配对）。
  void on_release() {
    int prev = count_.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 0) {
      // 防御性：release 多于 produce 属编程错误，回滚并标记溢出。
      count_.fetch_add(1, std::memory_order_acq_rel);
      overflow_.store(true, std::memory_order_release);
    }
  }

  int count() const { return count_.load(std::memory_order_acquire); }
  bool drained() const { return count() == 0; }
  bool overflow() const { return overflow_.load(std::memory_order_acquire); }

  // 等待计数归零；超时返回 false（未归零）。timeout_ms<=0 表示仅轮询一次。
  bool wait_drained(int timeout_ms) const {
    const int step_ms = 10;
    int elapsed = 0;
    while (count() > 0) {
      if (timeout_ms >= 0 && elapsed >= timeout_ms) return false;
      int chunk = (timeout_ms < 0) ? step_ms
                                    : ((timeout_ms - elapsed < step_ms)
                                           ? (timeout_ms - elapsed)
                                           : step_ms);
      if (chunk <= 0) chunk = 1;
      std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
      elapsed += chunk;
    }
    return true;
  }

  void reset() {
    count_.store(0, std::memory_order_release);
    overflow_.store(false, std::memory_order_release);
  }

 private:
  std::atomic<int> count_{0};
  std::atomic<bool> overflow_{false};
};

}  // namespace hzw

#endif  // HZW_VIDEO_INFLIGHT_FRAME_TRACKER_H
