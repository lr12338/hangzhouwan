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

  // 4) muxer-only 重连 PTS 续接不变量（回归：1a1a7b2 PTS reset bug）。
  //    muxer-only 重连不重建编码器，编码器 DTS 续接；PTS 必须续接（不可 reset），
  //    否则 PTS 归零 < 续接 DTS 致 FLV av_interleaved_write_frame 永久失败。
  //    编码器 time_base=1/fps，max_b_frames=0 -> 输出 DTS == PTS（无 B 帧重排）。
  {
    OutputPtsSequence pts;
    // 编码器 DTS 从 0 开始，与 PTS 同步递增（IPPP 无 B 帧）。
    int64_t encoder_dts = 0;
    int64_t prev = -1;
    // 模拟 1000 帧正常输出。
    for (int i = 0; i < 1000; ++i) {
      int64_t out = pts.next();
      CHECK(out == encoder_dts);  // PTS == DTS（IPPP）
      CHECK(out > prev);
      prev = out;
      ++encoder_dts;
    }

    // 模拟 RTMP 写帧失败 -> muxer-only 重连（编码器不重建，DTS 续接）。
    // 修复后：不调用 pts.reset()，PTS 续接。
    // （旧 bug：调用 pts.reset() -> PTS 归零 -> pts(0) < dts(1000) -> 永久失败）
    int64_t dts_before = encoder_dts;  // 1000

    // muxer-only 重连后继续输出 500 帧。
    for (int i = 0; i < 500; ++i) {
      int64_t out = pts.next();
      CHECK(out >= dts_before);   // PTS 续接，绝不归零
      CHECK(out == encoder_dts);  // 仍与编码器 DTS 同步
      CHECK(out > prev);
      prev = out;
      ++encoder_dts;
    }
    CHECK(pts.current() == 1500);

    // 反证：若错误地调用 reset()，PTS 会回到 0 < dts_before(1000) -> 必然 pts<dts。
    {
      OutputPtsSequence buggy;
      for (int i = 0; i < 1000; ++i) buggy.next();  // dts 到 1000
      buggy.reset();                                  // 错误 reset（旧 bug）
      int64_t bad_pts = buggy.next();                 // 0
      CHECK(bad_pts == 0);
      CHECK(bad_pts < 1000);  // 0 < 1000 = dts -> FLV 拒绝（这就是 bug）
    }
  }

  if (g_failures == 0) std::cout << "通过 | 输出 PTS 单调性单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
