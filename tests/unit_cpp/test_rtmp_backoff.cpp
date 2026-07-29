// -*- coding: utf-8 -*-
// RTMP 退避策略单元测试：固定序列 1s,2s,5s,10s,30s封顶；成功后重置。
// 纯逻辑，不依赖网络/硬件。
#include <cstdint>
#include <iostream>
#include "video/video_sink.h"

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cerr << "失败 | " << #cond << " @行" << __LINE__ << "\n";    \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

int main() {
  using hzw::RtmpBackoffPolicy;

  // 1) 固定序列增长
  {
    RtmpBackoffPolicy p;
    CHECK(p.attempts() == 0);
    CHECK(p.on_failure() == 1000);   // 第1次：1s
    CHECK(p.on_failure() == 2000);   // 第2次：2s
    CHECK(p.on_failure() == 5000);   // 第3次：5s
    CHECK(p.on_failure() == 10000);  // 第4次：10s
    CHECK(p.on_failure() == 30000);  // 第5次：30s（封顶）
    CHECK(p.on_failure() == 30000);  // 第6次：仍30s
    CHECK(p.attempts() == 6);
    CHECK(p.last_backoff_ms() == 30000);
  }

  // 2) 成功后重置
  {
    RtmpBackoffPolicy p;
    p.on_failure();
    p.on_failure();
    p.on_failure();
    CHECK(p.attempts() == 3);
    p.on_success();
    CHECK(p.attempts() == 0);
    CHECK(p.last_backoff_ms() == 0);
    CHECK(p.on_failure() == 1000);  // 重置后从1s开始
  }

  // 3) 多轮重连场景
  {
    RtmpBackoffPolicy p;
    // 第一轮重连：失败2次后成功
    CHECK(p.on_failure() == 1000);
    CHECK(p.on_failure() == 2000);
    p.on_success();
    // 第二轮重连：应从1s重新开始
    CHECK(p.on_failure() == 1000);
    CHECK(p.on_failure() == 2000);
    CHECK(p.on_failure() == 5000);
    p.on_success();
    CHECK(p.attempts() == 0);
  }

  if (g_failures == 0) std::cout << "通过 | RTMP 退避策略单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
