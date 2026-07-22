// -*- coding: utf-8 -*-
// 时间调度单元测试：验证输出/推理间隔计算逻辑。
// RTSP 模式使用墙钟时间调度，不依赖源帧率整除关系。
// 纯逻辑，不依赖硬件/FFmpeg。
#include <cstdint>
#include <iostream>
#include "video/video_sink.h"  // OutputPtsSequence reset

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

  // 1) OutputPtsSequence reset：新 RTMP 会话 PTS 从 0 重新开始
  {
    OutputPtsSequence pts;
    CHECK(pts.next() == 0);
    CHECK(pts.next() == 1);
    CHECK(pts.next() == 2);
    pts.reset();
    CHECK(pts.next() == 0);  // 重置后从0开始
    CHECK(pts.next() == 1);
  }

  // 2) 时间调度间隔计算：output_fps=10 -> 100ms，inference_fps=5 -> 200ms
  {
    int output_fps = 10;
    int inference_fps = 5;
    int64_t output_interval_ms = 1000 / output_fps;
    int64_t infer_interval_ms = 1000 / inference_fps;
    CHECK(output_interval_ms == 100);
    CHECK(infer_interval_ms == 200);
  }

  // 3) 非 20fps 源的时间调度：source_fps=25 时仍可用 output_fps=10, inference_fps=5
  {
    int source_fps = 25;  // 真实摄像头可能 25fps
    int output_fps = 10;
    int inference_fps = 5;
    // RTSP 模式不要求整除
    int64_t output_interval = 1000 / output_fps;  // 100ms
    int64_t infer_interval = 1000 / inference_fps; // 200ms
    CHECK(output_interval == 100);
    CHECK(infer_interval == 200);
    // 25fps 源每帧 40ms，100ms 内约 2-3 帧，取最新一帧输出
    // 200ms 内约 5 帧，取最新一帧推理
    (void)source_fps;
  }

  // 4) 模拟时间调度选择逻辑：每 output_interval_ms 最多选一帧
  {
    int64_t output_interval = 100;  // 10fps
    int64_t last_output = 0;
    int frames[] = {0, 40, 80, 120, 160, 200, 240, 280, 320, 360, 400};
    int selected = 0;
    for (int t : frames) {
      bool should = (last_output == 0) || (t - last_output >= output_interval);
      if (should) {
        last_output = t;
        ++selected;
      }
    }
    // t=0(选), 40(不), 80(不), 120(选,120-0>=100), 160(不), 200(不),
    // 240(选,240-120>=100), 280(不), 320(不), 360(选,360-240>=100), 400(选,400-360>=100)
    CHECK(selected == 5);
  }

  // 5) 重连后重置调度：last_output=0 时第一帧必选
  {
    int64_t output_interval = 100;
    int64_t last_output = 0;  // 重连后重置
    int t = 5000;  // 5秒后的第一帧
    bool should = (last_output == 0) || (t - last_output >= output_interval);
    CHECK(should);  // 必须选
  }

  if (g_failures == 0) std::cout << "通过 | 时间调度单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
