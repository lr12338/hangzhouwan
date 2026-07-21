// -*- coding: utf-8 -*-
// =============================================================================
// SingleStreamPipeline：单路视频推理管线（阶段4）。
//
// 链路：test.mp4 -> h264_bm 硬解 -> 最新帧队列(容量1,丢旧) -> 按频率推理(复用snapshot)
//       -> 绘框 -> in-place 写回 NV12 -> h264_bm 硬编 -> 本地输出文件
//
// 线程模型：解码线程 / 处理线程 / 编码线程，两级容量1丢旧队列解耦。
//   - 解码线程仅推送“输出帧”（按 output_fps 抽帧），避免无效预处理堆积。
//   - 处理线程做 NV12<->RGB(sws) + 预处理(复用 BmrtDetector) + 推理 + 绘框。
//   - 编码线程消费 AVFrame（bm_image）送硬编并封装。
// BmrtDetector 全程只创建一次。响应 SIGINT/SIGTERM/EOF/错误，统一释放资源。
// =============================================================================
#ifndef HZW_PIPELINE_SINGLE_STREAM_PIPELINE_H
#define HZW_PIPELINE_SINGLE_STREAM_PIPELINE_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include "inference/bmrt_detector.h"
#include "monitoring/pipeline_metrics.h"
#include "pipeline/detection_snapshot.h"
#include "video/latest_frame_queue.h"
#include "video/video_sink.h"
#include "video/video_source.h"

namespace hzw {

struct PipelineConfig {
  std::string input_path;
  std::string output_path;
  std::string bmodel_path;
  int device = 0;
  std::string decoder = "h264_bm";
  std::string encoder = "h264_bm";
  int source_fps = 20;
  int output_fps = 10;
  int inference_fps = 5;
  int bitrate_kbps = 800;
  int gop = 20;
  int queue_size = 1;
  float conf = 0.1f;
  float iou = 0.1f;
  int result_ttl_ms = 1000;
  int loop = 1;            // 0 = 无限循环
  int max_seconds = 0;     // 0 = 不限时（由 loop/信号控制）
  int metrics_interval_sec = 10;
  // 校验配置合法性；返回 false 时 err 给出原因。
  bool validate(std::string& err) const;
};

class SingleStreamPipeline {
 public:
  SingleStreamPipeline();
  ~SingleStreamPipeline();

  // 启动管线（阻塞直到结束：EOF/loop 用尽/信号/错误）。返回退出码。
  int run(const PipelineConfig& cfg);

  // 请求停止（信号处理调用）。
  void request_stop();

  const PipelineMetrics& metrics() const { return metrics_; }

 private:
  void decode_loop(const PipelineConfig& cfg);
  void process_loop(const PipelineConfig& cfg);
  void encode_loop(const PipelineConfig& cfg);
  void metrics_loop(const PipelineConfig& cfg);

  PipelineConfig cfg_;
  SophonVideoSource source_;
  SophonVideoSink sink_;
  std::unique_ptr<BmrtDetector> detector_;
  SnapshotStore snapshot_;
  PipelineMetrics metrics_;

  using FrameQueue = LatestFrameQueue<VideoFrame>;
  std::unique_ptr<FrameQueue> q_decode_;   // 解码 -> 处理
  std::unique_ptr<FrameQueue> q_encode_;   // 处理 -> 编码

  std::thread t_decode_, t_process_, t_encode_, t_metrics_;
  std::atomic<bool> stop_{false};
  std::atomic<int> exit_code_{0};
  int source_w_ = 0;
  int source_h_ = 0;
  int64_t start_ms_ = 0;
  void set_error(int code, const std::string& msg);
};

}  // namespace hzw

#endif  // HZW_PIPELINE_SINGLE_STREAM_PIPELINE_H
