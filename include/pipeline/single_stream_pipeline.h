// -*- coding: utf-8 -*-
// =============================================================================
// SingleStreamPipeline：单路视频推理管线（阶段4）。
//
// 链路：test.mp4/RTSP -> h264_bm 硬解 -> 最新帧队列(容量1,丢旧) -> 按频率推理(复用snapshot)
//       -> 绘框 -> in-place 写回 NV12 -> h264_bm 硬编 -> 本地文件/RTMP 输出
//
// 线程模型：解码线程 / 处理线程 / 编码线程，两级容量1丢旧队列解耦。
//   - 解码线程仅推送"输出帧"（文件模式按 output_fps 抽帧；RTSP 模式按墙钟时间调度），
//     避免无效预处理堆积。
//   - 处理线程做 NV12<->RGB(sws) + 预处理(复用 BmrtDetector) + 推理 + 绘框。
//   - 编码线程消费 AVFrame（bm_image）送硬编并封装（本地文件或 RTMP）。
// BmrtDetector 全程只创建一次。响应 SIGINT/SIGTERM/EOF/错误，统一释放资源。
//
// 阶段4.3：
//   - RTSP 模式使用单调时间调度（steady_clock），不依赖源帧率整除关系。
//   - 支持 RTMP 网络输出（sink_type=rtmp），RTMP 重连不重新加载 bmodel/RTSP。
//   - 支持禁区过滤（DetectionRegionFilter，NMS 后绘框前）。
// =============================================================================
#ifndef HZW_PIPELINE_SINGLE_STREAM_PIPELINE_H
#define HZW_PIPELINE_SINGLE_STREAM_PIPELINE_H

#include <atomic>
#include <memory>
#include <string>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "inference/bmrt_detector.h"
#include "monitoring/pipeline_metrics.h"
#include "pipeline/detection_snapshot.h"
#include "pipeline/enriched_snapshot.h"
#include "pipeline/async_jsonl_writer.h"
#include "video/latest_frame_queue.h"
#include "video/bmcv_processor.h"
#include "video/detection_region_filter.h"
#include "video/video_sink.h"
#include "business/business_enrichment_client.h"
#include "video/video_source.h"

namespace hzw {

enum class PipelineLifecycle { STARTING = 0, RUNNING = 1, EXITED = 2 };

// 稳定进程退出分类。systemd 与 Supervisor 依赖这些值，禁止散落魔法数字。
constexpr int kPipelineExitRuntime = 1;
constexpr int kPipelineExitHardware = 70;
constexpr int kPipelineExitEndpointUnavailable = 75;
constexpr int kPipelineExitConfiguration = 78;

// 健康接口只公开脱敏、限长后的错误文本。
std::string redact_pipeline_error(const std::string& message);

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
  int output_width = 1280;
  int output_height = 720;
  int bitrate_kbps = 1200;
  int gop = 20;
  int extra_frame_buffer_num = 20;  // 解码输出缓冲池大小（>= 编码器保留帧数，防 bm_image 池死锁）
  int queue_size = 1;
  float conf = 0.1f;
  float iou = 0.1f;
  int result_ttl_ms = 1000;
  int loop = 1;            // 0 = 无限循环
  int max_seconds = 0;     // 0 = 不限时（由 loop/信号控制）
  int metrics_interval_sec = 10;
  int jitter_buffer_size = 3;  // 抖动缓冲帧数（吸收RTSP突发，0=禁用）
  bool enable_business = false;          // 启用业务增强（坐标+AIS）
  std::string business_socket = "/tmp/hangzhouwan-business.sock";
  std::string event_directory = "/data/hangzhouwan/events";
  int event_rotate_size_mb = 50;
  int event_rotate_seconds = 3600;
  int event_retention_days = 7;
  int event_sync_seconds = 5;
  int event_queue_max_mb = 4;
  int event_disk_warn_mb = 2048;
  int event_disk_stop_mb = 1024;
  int request_timeout_ms = 200;           // Sidecar 请求超时（毫秒）
  std::string coordinate_mode = "sklearn";  // 坐标模式（JSONL/日志用）
  std::string preprocess = "cpu";   // cpu | bmcv
  std::string draw_mode = "cpu";    // cpu | bmcv | none
  // RTSP 输入（阶段4.2/4.3）
  std::string source_type = "file";  // file | rtsp
  std::string input_env;               // RTSP: 输入 URL 环境变量名（可选，URL 不入命令行/日志）
  std::string rtsp_transport = "tcp"; // tcp | udp
  int64_t rtsp_stimeout_us = 5000000;  // socket TCP I/O 超时（微秒）
  int rtsp_max_reconnect = -1;         // -1=无限 0=不重连 >0=上限
  int64_t rtsp_initial_backoff_ms = 1000;
  int64_t rtsp_max_backoff_ms = 30000;
  int rtsp_drain_timeout_ms = 5000;  // 重连前等待在途帧归零的超时
  // 输出 sink 类型（阶段4.3）
  std::string sink_type = "file";      // file | rtmp
  // StreamProfile（阶段4.3：业务配置迁移）
  std::string stream_id;
  std::string coordinate_model_path;
  double camera_param = 0.0;
  // 禁区过滤（阶段4.3：保留配置，参考分辨率由源探测决定）
  bool enable_region_filter = false;
  int region_ref_width = 0;   // 禁区坐标参考分辨率宽度（0=不换算）
  int region_ref_height = 0;  // 禁区坐标参考分辨率高度
  std::vector<Rect> forbidden_rectangles;
  std::vector<std::vector<Point>> forbidden_polygons;
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
  PipelineLifecycle lifecycle() const {
    return static_cast<PipelineLifecycle>(lifecycle_.load());
  }
  int pipeline_exit_code() const { return exit_code_.load(); }
  std::string failure_stage() const;
  std::string last_error() const;
  int source_fps() const { return source_.fps(); }

  // 设备资源致命（源或编码器 VPU ENOMEM/初始化失败）。致命时退出码=70。
  bool resource_fatal() const;
  std::string fatal_reason() const;

  // Business 连接状态（供健康接口读取）
  EnrichmentState business_state() const {
    return business_client_ ? business_client_->state() : EnrichmentState::DETECTION_ONLY;
  }
  int business_degrade_count() const {
    return business_client_ ? business_client_->degrade_count() : 0;
  }
  int business_recover_count() const {
    return business_client_ ? business_client_->recover_count() : 0;
  }
  bool business_link_healthy() const {
    return business_client_ ? business_client_->link_healthy() : false;
  }
  int64_t business_timeout_count() const {
    return business_client_ ? business_client_->timeout_count() : 0;
  }
  int64_t business_error_count() const {
    return business_client_ ? business_client_->error_count() : 0;
  }
  AsyncJsonlWriterStatus event_writer_status() const {
    return event_writer_ ? event_writer_->status() : AsyncJsonlWriterStatus{};
  }

 private:
  void capture_loop(const PipelineConfig& cfg);
  void decode_loop(const PipelineConfig& cfg);
  void process_loop(const PipelineConfig& cfg);
  void encode_loop(const PipelineConfig& cfg);
  void metrics_loop(const PipelineConfig& cfg);

  PipelineConfig cfg_;
  SophonVideoSource source_;
  SophonVideoSink sink_;
  std::unique_ptr<BmrtDetector> detector_;
  SnapshotStore snapshot_;
  EnrichedSnapshotStore enriched_snapshot_;
  PipelineMetrics metrics_;
  BmcvProcessor bmcv_;
  DetectionRegionFilter region_filter_;
  std::unique_ptr<BusinessEnrichmentClient> business_client_;
  std::unique_ptr<AsyncJsonlWriter> event_writer_;

  using FrameQueue = LatestFrameQueue<VideoFrame>;
  std::unique_ptr<FrameQueue> q_decode_;   // 解码 -> 处理
  std::unique_ptr<FrameQueue> q_encode_;   // 处理 -> 编码

  std::thread t_capture_, t_decode_, t_process_, t_encode_, t_metrics_;
  // 抖动缓冲（capture_loop -> decode_loop 调度）
  std::mutex jitter_mutex_;
  std::condition_variable jitter_cv_;
  std::deque<VideoFrame> jitter_buf_;
  bool capture_done_ = false;
  std::atomic<bool> stop_{false};
  std::atomic<int> exit_code_{0};
  std::atomic<int> lifecycle_{
      static_cast<int>(PipelineLifecycle::STARTING)};
  mutable std::mutex failure_mutex_;
  std::string failure_stage_;
  std::string last_error_;
  std::atomic<bool> rtsp_draining_{false};  // RTSP 重连前快速排空缓冲
  int source_w_ = 0;
  int source_h_ = 0;
  int64_t start_ms_ = 0;
  int last_epoch_ = 0;  // 上次处理的源 epoch，用于重连后清除过期检测结果
  // RTSP 时间调度状态（阶段4.3）
  int64_t last_output_ms_ = 0;   // 上次输出帧的墙钟时间
  int64_t last_infer_ms_ = 0;    // 上次推理的墙钟时间
  void set_error(int code, const std::string& msg);
  int fail_initialization(int code, const std::string& stage,
                          const std::string& msg);
  void set_failure_details(const std::string& stage,
                           const std::string& msg);
  // 资源致命：强制设置退出码 70 并停止（覆盖先前非致命错误码），停止双路。
  void set_resource_fatal(const std::string& msg);
  // RTSP 重连协调：清空 jitter/decode/encode 队列、等待在途帧归零后调用源重连。
  bool coordinate_rtsp_reconnect(const PipelineConfig& cfg, std::string& err);
  void drain_jitter_locked();
};

}  // namespace hzw

#endif  // HZW_PIPELINE_SINGLE_STREAM_PIPELINE_H
