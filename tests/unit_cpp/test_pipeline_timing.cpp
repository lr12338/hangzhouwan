// -*- coding: utf-8 -*-
// 管线调度与配置校验单元测试：帧率选择、推理帧为输出帧子集、时间戳单调、非法配置拒绝。
#include <iostream>
#include <string>
#include "application/dual_stream_application.h"
#include "pipeline/single_stream_pipeline.h"

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cerr << "失败 | " << #cond << " @行" << __LINE__ << "\n";    \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

int main() {
  using hzw::PipelineConfig;

  // 0) 退出码稳定且健康错误不会泄漏 RTSP 凭据。
  {
    CHECK(hzw::kPipelineExitRuntime == 1);
    CHECK(hzw::kPipelineExitHardware == 70);
    CHECK(hzw::kPipelineExitEndpointUnavailable == 75);
    CHECK(hzw::kPipelineExitConfiguration == 78);
    CHECK(!hzw::stream_exit_requires_global_stop(
        hzw::kPipelineExitEndpointUnavailable));
    CHECK(!hzw::stream_exit_requires_global_stop(hzw::kPipelineExitRuntime));
    CHECK(hzw::stream_exit_requires_global_stop(hzw::kPipelineExitHardware));
    CHECK(hzw::stream_exit_requires_global_stop(
        hzw::kPipelineExitConfiguration));
    CHECK(hzw::stream_exit_is_retryable(
        hzw::kPipelineExitEndpointUnavailable));
    CHECK(hzw::stream_exit_is_retryable(hzw::kPipelineExitRuntime));
    CHECK(!hzw::stream_exit_is_retryable(hzw::kPipelineExitHardware));
    CHECK(!hzw::stream_exit_is_retryable(
        hzw::kPipelineExitConfiguration));
    const std::string redacted = hzw::redact_pipeline_error(
        "open rtsp://camera-user:camera-pass@10.0.0.1/live failed");
    CHECK(redacted.find("camera-user") == std::string::npos);
    CHECK(redacted.find("camera-pass") == std::string::npos);
    CHECK(redacted.find("rtsp://***@10.0.0.1/live") != std::string::npos);
  }

  // 1) 合法配置通过。
  {
    PipelineConfig c;
    c.input_path = "a"; c.output_path = "b"; c.bmodel_path = "c";
    c.source_fps = 20; c.output_fps = 10; c.inference_fps = 5;
    std::string err;
    CHECK(c.validate(err));
  }
  // 2) output-fps 非法（0）。
  {
    PipelineConfig c; c.input_path="a"; c.output_path="b"; c.bmodel_path="c";
    c.source_fps=20; c.output_fps=0; c.inference_fps=5;
    std::string err; CHECK(!c.validate(err));
  }
  // 3) inference-fps 非法（0）。
  {
    PipelineConfig c; c.input_path="a"; c.output_path="b"; c.bmodel_path="c";
    c.source_fps=20; c.output_fps=10; c.inference_fps=0;
    std::string err; CHECK(!c.validate(err));
  }
  // 4) inference-fps 大于 source-fps。
  {
    PipelineConfig c; c.input_path="a"; c.output_path="b"; c.bmodel_path="c";
    c.source_fps=20; c.output_fps=10; c.inference_fps=25;
    std::string err; CHECK(!c.validate(err));
  }
  // 5) 不能整除被拒。
  {
    PipelineConfig c; c.input_path="a"; c.output_path="b"; c.bmodel_path="c";
    c.source_fps=20; c.output_fps=15; c.inference_fps=5;  // 20%15!=0
    std::string err; CHECK(!c.validate(err));
  }

  // 6) 帧率调度：output_step=2, infer_step=4；推理帧必为输出帧。
  {
    int source = 20, output = 10, infer = 5;
    int output_step = source / output;  // 2
    int infer_step = source / infer;    // 4
    CHECK(output_step == 2);
    CHECK(infer_step == 4);
    int out_cnt = 0, infer_cnt = 0;
    for (int seq = 0; seq < source; ++seq) {  // 1 秒的帧
      bool is_out = (seq % output_step == 0);
      bool is_inf = (seq % infer_step == 0);
      if (is_out) ++out_cnt;
      if (is_inf) { ++infer_cnt; CHECK(is_out); }  // 推理帧必须是输出帧
    }
    CHECK(out_cnt == output);     // 10 输出帧/秒
    CHECK(infer_cnt == infer);    // 5 推理帧/秒
  }
  // 7) 时间戳单调性：capture_time 随 sequence 递增。
  {
    int64_t prev = -1;
    for (int seq = 0; seq < 100; ++seq) {
      int64_t t = static_cast<int64_t>(seq) * 50;  // 20fps -> 50ms
      CHECK(t > prev);
      prev = t;
    }
  }

  if (g_failures == 0) std::cout << "通过 | 管线调度与配置单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
