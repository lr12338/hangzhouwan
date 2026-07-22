// -*- coding: utf-8 -*-
#include "application/dual_stream_application.h"

#include <chrono>
#include <cstdio>

namespace hzw {

namespace {
int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

DualStreamApplication::~DualStreamApplication() {
  request_stop();
  if (t_a_.joinable()) t_a_.join();
  if (t_b_.joinable()) t_b_.join();
  if (t_metrics_.joinable()) t_metrics_.join();
}

void DualStreamApplication::request_stop() {
  bool was = stop_.exchange(true);
  if (!was) {
    pipeline_a_.request_stop();
    pipeline_b_.request_stop();
  }
}

void DualStreamApplication::stream_thread(const PipelineConfig& cfg,
                                          SingleStreamPipeline& pipeline,
                                          std::atomic<int>& exit_code) {
  try {
    exit_code.store(pipeline.run(cfg));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "错误 | [%s] 线程异常 | %s\n",
                 cfg.stream_id.c_str(), e.what());
    std::fflush(stderr);
    exit_code.store(99);
  }
  // 任一路结束不直接杀死另一路；由 metrics_loop 的超时或信号统一停止。
}

int DualStreamApplication::run(const DualStreamConfig& cfg) {
  std::fprintf(stdout, "信息 | 双路 | 启动 detector_mode=%s max_seconds=%d\n",
               cfg.detector_mode.c_str(), cfg.max_seconds);
  std::fflush(stdout);

  start_ms_ = now_ms();
  stop_.store(false);
  exit_code_a_.store(0);
  exit_code_b_.store(0);

  // A/B 按需启动（单路时不启动另一路）
  if (cfg.run_a) {
    t_a_ = std::thread([this, &cfg] { stream_thread(cfg.stream_a, pipeline_a_, exit_code_a_); });
  } else {
    exit_code_a_.store(0);
  }
  if (cfg.run_b) {
    t_b_ = std::thread([this, &cfg] { stream_thread(cfg.stream_b, pipeline_b_, exit_code_b_); });
  } else {
    exit_code_b_.store(0);
  }
  t_metrics_ = std::thread([this, &cfg] { metrics_loop(cfg); });

  if (t_a_.joinable()) t_a_.join();
  if (t_b_.joinable()) t_b_.join();
  stop_.store(true);
  t_metrics_.join();

  int rc_a = exit_code_a_.load();
  int rc_b = exit_code_b_.load();
  int rc = 0;
  if (cfg.run_a && rc_a != 0) rc = 1;
  if (cfg.run_b && rc_b != 0) rc = 1;
  std::fprintf(stdout, "信息 | 双路 | 结束 A退出码=%d B退出码=%d 耗时=%llds\n",
               rc_a, rc_b, static_cast<long long>((now_ms() - start_ms_) / 1000));
  std::fflush(stdout);
  return rc;
}

void DualStreamApplication::metrics_loop(const DualStreamConfig& cfg) {
  while (!stop_.load()) {
    for (int i = 0; i < cfg.metrics_interval_sec * 10 && !stop_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (stop_.load()) break;

    // 双路汇总
    const auto& ma = pipeline_a_.metrics();
    const auto& mb = pipeline_b_.metrics();
    std::fprintf(stdout,
        "信息 | 双路汇总 | A:输出=%lld 推理=%lld 丢帧=%lld  "
        "B:输出=%lld 推理=%lld 丢帧=%lld  "
        "A推理P95=%.0fms B推理P95=%.0fms\n",
        static_cast<long long>(ma.output_frames.load()),
        static_cast<long long>(ma.inference_count.load()),
        static_cast<long long>(ma.dropped_frames.load()),
        static_cast<long long>(mb.output_frames.load()),
        static_cast<long long>(mb.inference_count.load()),
        static_cast<long long>(mb.dropped_frames.load()),
        0.0, 0.0);  // P95 由各路 summary 输出
    std::fflush(stdout);

    if (cfg.max_seconds > 0 && (now_ms() - start_ms_) / 1000 >= cfg.max_seconds) {
      std::fprintf(stdout, "信息 | 双路 | 达到 %d 秒，主动停止\n", cfg.max_seconds);
      std::fflush(stdout);
      request_stop();
      break;
    }
  }
}

}  // namespace hzw
