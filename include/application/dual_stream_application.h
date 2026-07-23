// -*- coding: utf-8 -*-
// =============================================================================
// DualStreamApplication：A/B 双路并发视频推理应用（阶段4.4）。
//
// 管理 two SingleStreamPipeline 实例，各自独立 RTSP/RTMP/推理/编码线程。
// 全局 SIGINT/SIGTERM 同时停止两路；任一路失败不杀另一路。
// 每路拥有独立 metrics、snapshot 和 source/sink epoch。
// 日志带 stream_id=A|B 前缀。
//
// detector_mode:
//   per_stream       — A/B 各加载一份 bmodel，独立 BMRuntime 上下文，并发推理。
//   shared_serialized — 单份 bmodel + 互斥推理队列，A->B 公平调度（后续实现）。
// =============================================================================
#ifndef HZW_APPLICATION_DUAL_STREAM_APPLICATION_H
#define HZW_APPLICATION_DUAL_STREAM_APPLICATION_H

#include <atomic>
#include <string>
#include <thread>
#include "pipeline/single_stream_pipeline.h"
#include "monitoring/video_health_server.h"

namespace hzw {

struct DualStreamConfig {
  PipelineConfig stream_a;
  PipelineConfig stream_b;
  int max_seconds = 0;
  int metrics_interval_sec = 10;
  std::string detector_mode = "per_stream";
  bool run_a = true;   // 是否启动 A 路
  bool run_b = true;   // 是否启动 B 路
};

class DualStreamApplication {
 public:
  DualStreamApplication() = default;
  ~DualStreamApplication();

  // 获取健康状态（供外部查询）
  const VideoHealthServer& health_server() const { return health_server_; }

  // 启动双路（阻塞直到两路结束）。返回 0=全部成功，非 0=至少一路失败。
  int run(const DualStreamConfig& cfg);

  // 全局停止（信号处理调用）。
  void request_stop();

 private:
  void stream_thread(const PipelineConfig& cfg, SingleStreamPipeline& pipeline,
                     std::atomic<int>& exit_code);
  void metrics_loop(const DualStreamConfig& cfg);

  SingleStreamPipeline pipeline_a_;
  SingleStreamPipeline pipeline_b_;
  std::thread t_a_, t_b_, t_metrics_;
  std::atomic<bool> stop_{false};
  std::atomic<int> exit_code_a_{0};
  std::atomic<int> exit_code_b_{0};
  int64_t start_ms_ = 0;
  VideoHealthServer health_server_;
  int64_t prev_output_a_ = 0;
  int64_t prev_infer_a_ = 0;
  int64_t prev_output_b_ = 0;
  int64_t prev_infer_b_ = 0;
  int64_t prev_sample_ms_ = 0;
  // 上次采样时的重连累计值，用于计算单窗口重连增量（重连风暴检测）
  int64_t prev_reconnect_a_ = 0;
  int64_t prev_reconnect_b_ = 0;
};

}  // namespace hzw

#endif  // HZW_APPLICATION_DUAL_STREAM_APPLICATION_H
