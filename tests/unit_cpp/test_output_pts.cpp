// -*- coding: utf-8 -*-
// 输出 PTS 单调性单元测试：输出帧序严格递增；源 PTS 回退/RTSP 重连跳变不影响输出 PTS。
// 不依赖硬件（直接测试 OutputPtsSequence 逻辑）。
#include <cstdint>
#include <iostream>
#include <vector>
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
  using hzw::OutputPtsSequence;

  // 1) 严格递增：连续 next() 得到 0,1,2,...
  {
    OutputPtsSequence pts;
    int64_t prev = -1;
    for (int i = 0; i < 1000; ++i) {
      int64_t v = pts.next();
      CHECK(v == prev + 1);
      CHECK(v > prev);
      prev = v;
    }
    CHECK(pts.current() == 1000);
  }

  // 2) 源 PTS 回退/跳变不影响输出 PTS：模拟源 PTS 序列（含回退到 0 的重连场景），
  //    输出 PTS 仍按帧序严格递增（OutputPtsSequence 完全忽略源 PTS）。
  {
    OutputPtsSequence pts;
    std::vector<int64_t> src_pts = {1000, 2000, 3000, 0, 1000, 2000};  // 第4帧源 PTS 跳回 0（重连）
    int64_t prev = -1;
    for (int64_t src_pt : src_pts) {
      (void)src_pt;
      int64_t out = pts.next();
      CHECK(out > prev);
      prev = out;
    }
  }

  // 3) 大量帧后无回退/重复。
  {
    OutputPtsSequence pts;
    std::vector<int64_t> seen;
    seen.reserve(100000);
    for (int i = 0; i < 100000; ++i) seen.push_back(pts.next());
    bool monotonic = true;
    for (size_t i = 1; i < seen.size(); ++i) {
      if (seen[i] <= seen[i - 1]) { monotonic = false; break; }
    }
    CHECK(monotonic);
    CHECK(seen.front() == 0);
    CHECK(seen.back() == 99999);
  }

  if (g_failures == 0) std::cout << "通过 | 输出 PTS 单调性单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
