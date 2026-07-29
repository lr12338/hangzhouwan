// -*- coding: utf-8 -*-
// InflightFrameTracker 单元测试（纯逻辑，不依赖 FFmpeg/硬件）。
// 验证：produce/release 配对计数、归零判定、超时语义、溢出保护。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include "video/inflight_frame_tracker.h"
#include <iostream>

using hzw::InflightFrameTracker;

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "失败 | " << #cond << " @行" << __LINE__ << "\n";         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

int main() {
  // ---- 1) 基本计数与归零 ----
  {
    InflightFrameTracker t;
    CHECK(t.count() == 0);
    CHECK(t.drained());
    t.on_produce(); t.on_produce(); t.on_produce();
    CHECK(t.count() == 3);
    CHECK(!t.drained());
    t.on_release(); t.on_release();
    CHECK(t.count() == 1);
    t.on_release();
    CHECK(t.count() == 0);
    CHECK(t.drained());
    CHECK(!t.overflow());
  }

  // ---- 2) 溢出保护：release 多于 produce 不应跌至负数 ----
  {
    InflightFrameTracker t;
    t.on_release();  // 未 produce 即 release：编程错误，应被拦截
    CHECK(t.count() == 0);   // 回滚，不变成 -1
    CHECK(t.overflow());
  }

  // ---- 3) wait_drained：已归零立即返回 ----
  {
    InflightFrameTracker t;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = t.wait_drained(1000);
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    CHECK(ok);
    CHECK(dt < 50);  // 立即返回
  }

  // ---- 4) wait_drained：未归零超时返回 false ----
  {
    InflightFrameTracker t;
    t.on_produce();  // 永不 release
    auto t0 = std::chrono::steady_clock::now();
    bool ok = t.wait_drained(150);
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    CHECK(!ok);
    CHECK(dt >= 140);
    CHECK(t.count() == 1);
  }

  // ---- 5) 并发 produce/release 后归零（模拟多线程在途帧释放）----
  {
    InflightFrameTracker t;
    const int N = 200;
    for (int i = 0; i < N; ++i) t.on_produce();
    std::thread th([&] {
      for (int i = 0; i < N; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        t.on_release();
      }
    });
    CHECK(t.wait_drained(5000));  // 应在 5s 内归零
    CHECK(t.drained());
    th.join();
  }

  // ---- 6) reset ----
  {
    InflightFrameTracker t;
    t.on_produce(); t.on_produce();
    t.reset();
    CHECK(t.count() == 0);
    CHECK(!t.overflow());
  }

  if (g_failures == 0) std::cout << "通过 | InflightFrameTracker 单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
