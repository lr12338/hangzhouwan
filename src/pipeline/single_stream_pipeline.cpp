// -*- coding: utf-8 -*-
#include "pipeline/single_stream_pipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include "image_io/jpeg_io.h"

namespace hzw {

namespace {
int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
double ms_between(int64_t a, int64_t b) { return static_cast<double>(b - a); }

// JSON 字符串转义（用于 JSONL 输出）。
std::string escape_json_str(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char ch : s) {
    switch (ch) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out;
}

bool preprocess_rgb_to_nchw(const Image& rgb, int input_w, int input_h,
                            std::vector<float>& out) {
  if (rgb.width != input_w || rgb.height != input_h) return false;
  const int plane = input_w * input_h;
  out.assign(static_cast<size_t>(3) * plane, 0.0f);
  const float inv = 1.0f / 255.0f;
  float* rp = out.data();
  float* gp = out.data() + plane;
  float* bp = out.data() + 2 * plane;
  const uint8_t* p = rgb.data.data();
  for (int i = 0; i < plane; ++i) {
    rp[i] = static_cast<float>(p[0]) * inv;
    gp[i] = static_cast<float>(p[1]) * inv;
    bp[i] = static_cast<float>(p[2]) * inv;
    p += 3;
  }
  return true;
}
}  // namespace

bool PipelineConfig::validate(std::string& err) const {
  const bool is_rtsp = (source_type == "rtsp");
  if (source_type != "file" && source_type != "rtsp") {
    err = "source_type 必须为 file 或 rtsp"; return false;
  }
  if (source_type == "file" && input_path.empty()) { err = "input_path 为空"; return false; }
  if (is_rtsp && input_path.empty() && input_env.empty()) {
    err = "RTSP 需通过 --input 或 --input-env 提供输入 URL"; return false;
  }
  if (output_path.empty()) { err = "output_path 为空"; return false; }
  if (bmodel_path.empty()) { err = "bmodel_path 为空"; return false; }
  if (source_fps <= 0) { err = "source_fps 非法"; return false; }
  if (output_fps <= 0) { err = "output_fps 非法"; return false; }
  if (inference_fps <= 0) { err = "inference_fps 非法"; return false; }
  if (inference_fps > output_fps) { err = "inference_fps 大于 output_fps"; return false; }
  if (!is_rtsp) {
    if (inference_fps > source_fps) { err = "inference_fps 大于 source_fps"; return false; }
    if (output_fps > source_fps) { err = "output_fps 大于 source_fps"; return false; }
    if (source_fps % output_fps != 0) { err = "source_fps 必须能被 output_fps 整除"; return false; }
    if (source_fps % inference_fps != 0) { err = "source_fps 必须能被 inference_fps 整除"; return false; }
    if (output_fps % inference_fps != 0) { err = "output_fps 必须能被 inference_fps 整除"; return false; }
  }
  if (queue_size < 1) { err = "queue_size 至少为 1"; return false; }
  if (conf < 0.0f || conf > 1.0f) { err = "conf 越界"; return false; }
  if (iou < 0.0f || iou > 1.0f) { err = "iou 越界"; return false; }
  if (bitrate_kbps <= 0) { err = "bitrate_kbps 非法"; return false; }
  if (gop <= 0) { err = "gop 非法"; return false; }
  if (result_ttl_ms < 0) { err = "result_ttl_ms 非法"; return false; }
  if (device < 0) { err = "device 非法"; return false; }
  if (preprocess != "cpu" && preprocess != "bmcv") {
    err = "preprocess 必须为 cpu 或 bmcv"; return false;
  }
  if (draw_mode != "cpu" && draw_mode != "bmcv" && draw_mode != "none") {
    err = "draw_mode 必须为 cpu、bmcv 或 none"; return false;
  }
  if (sink_type != "file" && sink_type != "rtmp") {
    err = "sink_type 必须为 file 或 rtmp"; return false;
  }
  if (is_rtsp) {
    RtspSourceOptions ro; ro.transport = rtsp_transport; ro.stimeout_us = rtsp_stimeout_us;
    ro.max_reconnect_attempts = rtsp_max_reconnect; ro.initial_backoff_ms = rtsp_initial_backoff_ms;
    ro.max_backoff_ms = rtsp_max_backoff_ms;
    if (!ro.validate(err)) return false;
  }
  if (jitter_buffer_size < 0) { err = "jitter_buffer_size 不能为负"; return false; }
  return true;
}

SingleStreamPipeline::SingleStreamPipeline() = default;
SingleStreamPipeline::~SingleStreamPipeline() {
  request_stop();
  if (business_jsonl_) { std::fclose(business_jsonl_); business_jsonl_ = nullptr; }
  if (t_capture_.joinable()) t_capture_.join();
  if (t_decode_.joinable()) t_decode_.join();
  if (t_process_.joinable()) t_process_.join();
  if (t_encode_.joinable()) t_encode_.join();
  if (t_metrics_.joinable()) t_metrics_.join();
}

void SingleStreamPipeline::set_error(int code, const std::string& msg) {
  if (exit_code_.load() == 0) {
    exit_code_ = code;
    std::fprintf(stderr, "错误 | 管线 | %s\n", msg.c_str());
    std::fflush(stderr);
  }
  request_stop();
}

void SingleStreamPipeline::request_stop() {
  source_.request_stop();
  sink_.request_stop();
  bool was = stop_.exchange(true);
  if (!was) {
    if (q_decode_) q_decode_->close();
    if (q_encode_) q_encode_->close();
    {
      std::lock_guard<std::mutex> lk(jitter_mutex_);
      capture_done_ = true;
    }
    jitter_cv_.notify_all();
  }
}

int SingleStreamPipeline::run(const PipelineConfig& cfg) {
  cfg_ = cfg;
  std::string err;
  if (!cfg_.validate(err)) {
    std::fprintf(stderr, "错误 | 配置 | %s\n", err.c_str());
    return 2;
  }

  metrics_.stream_id = cfg_.stream_id;
  if (!cfg_.stream_id.empty()) {
    std::fprintf(stdout, "信息 | StreamProfile | stream_id=%s\n", cfg_.stream_id.c_str());
    std::fflush(stdout);
  }

  if (cfg_.source_type == "rtsp") {
    std::string url;
    if (!cfg_.input_path.empty()) {
      url = cfg_.input_path;
    } else {
      std::string e;
      if (!resolve_input_env(cfg_.input_env, url, e)) {
        std::fprintf(stderr, "错误 | RTSP输入 | %s\n", e.c_str());
        return 3;
      }
    }
    RtspSourceOptions ro;
    ro.transport = cfg_.rtsp_transport;
    ro.stimeout_us = cfg_.rtsp_stimeout_us;
    ro.max_reconnect_attempts = cfg_.rtsp_max_reconnect;
    ro.initial_backoff_ms = cfg_.rtsp_initial_backoff_ms;
    ro.max_backoff_ms = cfg_.rtsp_max_backoff_ms;
    if (!source_.open_rtsp(url, cfg_.device, 20, cfg_.decoder, ro, err)) {
      std::fprintf(stderr, "错误 | 视频源 | %s\n", err.c_str());
      return 3;
    }
  } else {
    source_.set_loop(cfg_.loop);
    if (!source_.open(cfg_.input_path, cfg_.device, 20, cfg_.decoder, err)) {
      std::fprintf(stderr, "错误 | 视频源 | %s\n", err.c_str());
      return 3;
    }
  }
  source_w_ = source_.width();
  source_h_ = source_.height();

  detector_ = std::make_unique<BmrtDetector>(cfg_.device, cfg_.bmodel_path);
  if (!detector_ || !detector_->ok()) {
    std::fprintf(stderr, "错误 | 模型 | %s\n", detector_ ? detector_->last_error().c_str() : "分配失败");
    return 4;
  }
  std::fprintf(stdout, "信息 | 模型 | 网络=%s 输入=%s 输出=%s\n",
               detector_->net_name().c_str(), detector_->input_name().c_str(),
               detector_->output_name().c_str());
  std::fflush(stdout);

  bool want_bmcv = (cfg_.preprocess == "bmcv" || cfg_.draw_mode == "bmcv");
  if (want_bmcv) {
    std::string berr;
    if (bmcv_.init(detector_->handle(), source_w_, source_h_, berr)) {
      std::fprintf(stdout, "信息 | BMCV | 初始化成功 预处理=%s 绘制=%s\n",
                   cfg_.preprocess.c_str(), cfg_.draw_mode.c_str());
    } else {
      std::fprintf(stdout, "信息 | BMCV | 初始化失败(%s)，回退 CPU 路径\n", berr.c_str());
      cfg_.preprocess = "cpu";
      cfg_.draw_mode = (cfg_.draw_mode == "bmcv") ? "cpu" : cfg_.draw_mode;
    }
    std::fflush(stdout);
  }

  if (cfg_.enable_region_filter) {
    region_filter_.configure(cfg_.forbidden_rectangles, cfg_.forbidden_polygons,
                             cfg_.region_ref_width, cfg_.region_ref_height,
                             source_w_, source_h_);
    std::fprintf(stdout, "信息 | 禁区过滤 | 已启用 矩形=%zu 多边形=%zu 参考分辨率=%dx%d 实际=%dx%d\n",
                 cfg_.forbidden_rectangles.size(), cfg_.forbidden_polygons.size(),
                 cfg_.region_ref_width, cfg_.region_ref_height, source_w_, source_h_);
    std::fflush(stdout);
  }

  if (!sink_.open(cfg_.output_path, source_w_, source_h_, cfg_.output_fps,
                  cfg_.bitrate_kbps, cfg_.gop, cfg_.device, cfg_.encoder,
                  cfg_.sink_type, err)) {
    std::fprintf(stderr, "错误 | 视频输出 | %s\n", err.c_str());
    return 5;
  }

  // 业务增强初始化
  if (cfg_.enable_business) {
    business_client_ = std::make_unique<BusinessEnrichmentClient>();
    if (business_client_->connect(cfg_.business_socket)) {
      std::fprintf(stdout, "信息 | 业务 | Sidecar 连接成功 %s\n", cfg_.business_socket.c_str());
    } else {
      std::fprintf(stdout, "信息 | 业务 | Sidecar 连接失败，降级为仅检测\n");
    }
    if (!cfg_.business_jsonl_path.empty()) {
      business_jsonl_ = std::fopen(cfg_.business_jsonl_path.c_str(), "a");
      if (business_jsonl_) {
        std::fprintf(stdout, "信息 | 业务 | JSONL 输出 %s\n", cfg_.business_jsonl_path.c_str());
      }
    }
  }

  auto releaser = [](VideoFrame& f) { f.release(); };
  q_decode_ = std::make_unique<FrameQueue>(cfg_.queue_size, releaser);
  q_encode_ = std::make_unique<FrameQueue>(cfg_.queue_size, releaser);

  last_output_ms_ = 0;
  last_infer_ms_ = 0;
  capture_done_ = false;

  start_ms_ = now_ms();
  t_capture_ = std::thread([this] { capture_loop(cfg_); });
  t_decode_ = std::thread([this] { decode_loop(cfg_); });
  t_process_ = std::thread([this] { process_loop(cfg_); });
  t_encode_ = std::thread([this] { encode_loop(cfg_); });
  t_metrics_ = std::thread([this] { metrics_loop(cfg_); });

  t_capture_.join();
  {
    std::lock_guard<std::mutex> lk(jitter_mutex_);
    capture_done_ = true;
  }
  jitter_cv_.notify_all();
  t_decode_.join();
  q_decode_->close();
  t_process_.join();
  q_encode_->close();
  t_encode_.join();
  stop_.store(true);
  t_metrics_.join();

  metrics_.rtsp_reconnects.store(source_.reconnect_count());
  metrics_.rtmp_reconnects.store(sink_.rtmp_reconnect_count());

  sink_.close();
  source_.close();

  std::fprintf(stdout, "%s\n", metrics_.summary().c_str());
  std::fprintf(stdout, "信息 | 结束 | 输出帧=%lld 编码器=%s 容器=%s sink=%s RTSP重连=%d RTMP重连=%d 退出码=%d\n",
               static_cast<long long>(sink_.output_frames()),
               sink_.actual_encoder().c_str(), sink_.actual_container().c_str(),
               sink_.sink_type().c_str(), source_.reconnect_count(),
               sink_.rtmp_reconnect_count(), exit_code_.load());
  std::fflush(stdout);
  return exit_code_.load();
}

// capture_loop：持续从视频源读取帧，存入抖动缓冲。
// 抖动缓冲吸收 RTSP 突发到达，使 decode_loop 调度器能按 output_fps 稳定输出。
void SingleStreamPipeline::capture_loop(const PipelineConfig& cfg) {
  const bool is_rtsp = (cfg.source_type == "rtsp");
  const int64_t frame_interval_ms = is_rtsp ? 0 : (1000 / cfg.source_fps);
  const int64_t pace_start_ms = now_ms();
  int prev_epoch = -1;
  VideoFrame vf;
  std::string err;
  while (!stop_.load()) {
    int64_t read_start = now_ms();
    if (!source_.read(vf, err)) {
      if (err == "EOF") {
        std::fprintf(stdout, "信息 | 解码 | 正常 EOF，循环完成 %d 轮\n",
                     source_.loops_completed());
        std::fflush(stdout);
      } else if (err == "停止") {
        // 信号/限时主动停止
      } else {
        set_error(6, "解码失败: " + err);
      }
      break;
    }
    metrics_.read_success.fetch_add(1);
    metrics_.decoded_frames.fetch_add(1);
    metrics_.read_frames.store(source_.packets_read());
    metrics_.last_read_ms.store(now_ms());
    metrics_.record_read_ms(ms_between(read_start, now_ms()));

    // RTSP epoch 变化（重连）：清除过期缓冲帧
    if (is_rtsp && vf.source_epoch != prev_epoch) {
      prev_epoch = vf.source_epoch;
      std::lock_guard<std::mutex> lk(jitter_mutex_);
      while (!jitter_buf_.empty()) {
        jitter_buf_.front().release();
        jitter_buf_.pop_front();
      }
    }

    // 文件模式：按源帧率回放节流
    if (!is_rtsp) {
      const int64_t expected_ms = pace_start_ms + vf.sequence * frame_interval_ms;
      const int64_t actual_ms = now_ms();
      if (actual_ms < expected_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(expected_ms - actual_ms));
      }
    }

    // 推入抖动缓冲（满时丢最旧帧）
    {
      std::lock_guard<std::mutex> lk(jitter_mutex_);
      int max_buf = cfg.jitter_buffer_size > 0 ? cfg.jitter_buffer_size : 1;
      while (static_cast<int>(jitter_buf_.size()) >= max_buf) {
        jitter_buf_.front().release();
        jitter_buf_.pop_front();
        metrics_.decode_queue_dropped.fetch_add(1);
      }
      jitter_buf_.push_back(std::move(vf));
    }
    vf.frame = nullptr;
    jitter_cv_.notify_one();
  }
}

// decode_loop：调度输出。从抖动缓冲取帧，按 output_fps 墙钟时间调度（RTSP）
// 或序列号抽帧（文件），推入 q_decode。
// 关键改进：RTSP 模式仅在"到输出时间"时从缓冲取帧并输出，其余时间帧留在缓冲中，
// 避免突发到达时只输出 1 帧/突发的问题。
void SingleStreamPipeline::decode_loop(const PipelineConfig& cfg) {
  const bool is_rtsp = (cfg.source_type == "rtsp");
  const int output_step = is_rtsp ? 1 : (cfg.source_fps / cfg.output_fps);
  const int64_t output_interval_ms = is_rtsp ? (1000 / cfg.output_fps) : 0;
  int prev_epoch = -1;

  while (!stop_.load()) {
    if (is_rtsp) {
      const int64_t t = now_ms();
      const bool should_output =
          (last_output_ms_ == 0) || (t - last_output_ms_ >= output_interval_ms);
      if (!should_output) {
        int64_t wait = output_interval_ms - (t - last_output_ms_);
        if (wait > 10) wait = 10;
        std::this_thread::sleep_for(std::chrono::milliseconds(wait));
        continue;
      }
      // 到输出时间：从抖动缓冲取最旧帧（FIFO，保留突发帧）
      VideoFrame vf;
      {
        std::unique_lock<std::mutex> lk(jitter_mutex_);
        jitter_cv_.wait(lk, [this] {
          return !jitter_buf_.empty() || capture_done_ || stop_.load();
        });
        if (stop_.load()) break;
        if (jitter_buf_.empty() && capture_done_) break;
        if (jitter_buf_.empty()) continue;
        vf = std::move(jitter_buf_.front());
        jitter_buf_.pop_front();
      }
      if (vf.source_epoch != prev_epoch) {
        prev_epoch = vf.source_epoch;
      }
      metrics_.scheduled_output.fetch_add(1);
      metrics_.queued_frames.fetch_add(1);
      q_decode_->push(std::move(vf));
      last_output_ms_ = now_ms();
    } else {
      // 文件模式：从缓冲取帧，按序列号抽帧
      VideoFrame vf;
      {
        std::unique_lock<std::mutex> lk(jitter_mutex_);
        jitter_cv_.wait(lk, [this] {
          return !jitter_buf_.empty() || capture_done_ || stop_.load();
        });
        if (stop_.load()) break;
        if (jitter_buf_.empty() && capture_done_) break;
        if (jitter_buf_.empty()) continue;
        vf = std::move(jitter_buf_.front());
        jitter_buf_.pop_front();
      }
      const bool should_output = (vf.sequence % output_step == 0);
      if (should_output) {
        metrics_.scheduled_output.fetch_add(1);
        metrics_.queued_frames.fetch_add(1);
        q_decode_->push(std::move(vf));
      } else {
        vf.release();
      }
    }
  }
}

void SingleStreamPipeline::process_loop(const PipelineConfig& cfg) {
  const bool is_rtsp = (cfg.source_type == "rtsp");
  const int infer_step = is_rtsp ? 1 : (cfg.source_fps / cfg.inference_fps);
  const int64_t infer_interval_ms = 1000 / cfg.inference_fps;
  const int input_size = 640;
  const bool cpu_draw = (cfg.draw_mode == "cpu");
  const bool bmcv_draw = (cfg.draw_mode == "bmcv");
  const bool cpu_pre = (cfg.preprocess == "cpu");
  const bool bmcv_pre = (cfg.preprocess == "bmcv" && bmcv_.ready());

  SwsContext* nv12_to_rgb = nullptr;
  SwsContext* rgb_to_nv12 = nullptr;
  SwsContext* nv12_to_rgb640 = nullptr;
  if (cpu_draw) {
    nv12_to_rgb = sws_getContext(
        source_w_, source_h_, AV_PIX_FMT_NV12,
        source_w_, source_h_, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    rgb_to_nv12 = sws_getContext(
        source_w_, source_h_, AV_PIX_FMT_RGB24,
        source_w_, source_h_, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
  }
  if (cpu_pre) {
    nv12_to_rgb640 = sws_getContext(
        source_w_, source_h_, AV_PIX_FMT_NV12,
        640, 640, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
  }

  Image rgb;
  if (cpu_draw) {
    rgb.width = source_w_; rgb.height = source_h_;
    rgb.data.assign(static_cast<size_t>(source_w_) * source_h_ * 3, 0);
  }
  Image rgb640;
  if (cpu_pre) {
    rgb640.width = 640; rgb640.height = 640;
    rgb640.data.assign(static_cast<size_t>(640) * 640 * 3, 0);
  }
  std::vector<float> input, output;
  std::string berr;

  const int num_boxes = detector_->output_shape().size() >= 2 ? detector_->output_shape()[1] : 25200;
  const int num_vals = detector_->output_shape().size() >= 3 ? detector_->output_shape()[2] : 6;

  VideoFrame vf;
  while (!stop_.load() && q_decode_->pop(vf)) {
    if (!vf.frame) continue;
    if (vf.source_epoch != last_epoch_) {
      snapshot_.clear();
      enriched_snapshot_.clear();
      last_epoch_ = vf.source_epoch;
      last_infer_ms_ = 0;
    }
    const int64_t proc_start = now_ms();
    AVFrame* f = vf.frame;

    // 推理调度
    bool is_infer;
    if (is_rtsp) {
      const int64_t t = now_ms();
      is_infer = (last_infer_ms_ == 0) || (t - last_infer_ms_ >= infer_interval_ms);
      if (is_infer) last_infer_ms_ = t;
    } else {
      is_infer = (vf.sequence % infer_step == 0);
    }

    if (is_infer) {
      int64_t t0 = now_ms();
      bool pre_ok = false;
      if (bmcv_pre) {
        int64_t tvpp = now_ms();
        input.assign(static_cast<size_t>(detector_->input_element_count()), 0.0f);
        pre_ok = bmcv_.preprocess(f, input.data(), berr);
        metrics_.record_vpp_ms(ms_between(tvpp, now_ms()));
        if (!pre_ok) {
          set_error(9, "BMCV 预处理失败: " + berr);
        }
      } else {
        const uint8_t* src[2] = {f->data[0], f->data[1]};
        const int src_stride[2] = {f->linesize[0], f->linesize[1]};
        uint8_t* dst640[1] = {rgb640.data.data()};
        const int dst_stride640[1] = {640 * 3};
        int64_t tnorm0 = now_ms();
        sws_scale(nv12_to_rgb640, src, src_stride, 0, source_h_, dst640, dst_stride640);
        metrics_.record_vpp_ms(ms_between(tnorm0, now_ms()));
        int64_t tnorm1 = now_ms();
        pre_ok = preprocess_rgb_to_nchw(rgb640, input_size, input_size, input);
        metrics_.record_normalize_ms(ms_between(tnorm1, now_ms()));
      }
      metrics_.preprocess_count.fetch_add(1);
      if (pre_ok) {
        int64_t t1 = now_ms();
        if (detector_->infer(input, output)) {
          int64_t t2 = now_ms();
          std::vector<Detection> dets;
          postprocess_yolov7(output.data(), num_boxes, num_vals, source_w_, source_h_,
                             input_size, cfg.conf, cfg.iou, dets);
          int64_t t3 = now_ms();
          // 禁区过滤
          if (region_filter_.enabled()) {
            int64_t treg = now_ms();
            region_filter_.filter(dets);
            metrics_.record_region_ms(ms_between(treg, now_ms()));
          }
          metrics_.record_pre_ms(ms_between(t0, t1));
          metrics_.record_infer_ms(ms_between(t1, t2));
          metrics_.record_post_ms(ms_between(t2, t3));
          metrics_.inference_count.fetch_add(1);
          if (dets.empty()) metrics_.empty_detection.fetch_add(1);
          DetectionSnapshot snap;
          snap.source_sequence = vf.sequence;
          snap.source_pts = vf.pts;
          snap.generated_time_ms = t3;
          snap.detections = std::move(dets);
          snapshot_.update(snap);
          // 业务增强：坐标预测 + AIS 匹配（不阻塞视频路径）
          // 处理顺序：检测 -> NMS -> 禁区过滤 -> Sidecar 批量增强 ->
          //           EnrichedSnapshot 更新 -> 绘框 -> 编码推流
          EnrichedDetectionSnapshot esnap;
          esnap.source_sequence = vf.sequence;
          esnap.source_pts = vf.pts;
          esnap.generated_time_ms = t3;
          esnap.coordinate_mode = cfg.coordinate_mode;
          esnap.enrichment_status = EnrichmentState::DETECTION_ONLY;

          if (business_client_) {
            std::vector<DetectionBox> boxes;
            const auto& sd = snap.detections;
            for (size_t i = 0; i < sd.size(); ++i) {
              DetectionBox b;
              b.detection_id = static_cast<int>(i);
              b.score = sd[i].score;
              b.x1 = sd[i].x1; b.y1 = sd[i].y1;
              b.x2 = sd[i].x2; b.y2 = sd[i].y2;
              boxes.push_back(b);
            }
            std::vector<BusinessResult> bres;
            bool enrich_ok = business_client_->enrich(
                cfg.stream_id, vf.sequence, source_w_, source_h_,
                boxes, bres, cfg.request_timeout_ms);
            // 融合 Detection + BusinessResult（按 detection_id 索引）
            for (size_t i = 0; i < sd.size(); ++i) {
              const BusinessResult* rp = nullptr;
              for (const auto& r : bres) {
                if (r.detection_id == static_cast<int>(i)) { rp = &r; break; }
              }
              if (rp) {
                esnap.detections.emplace_back(sd[i], *rp);
              } else {
                esnap.detections.emplace_back(sd[i]);
              }
            }
            esnap.enrichment_status = business_client_->state();
            // 写入合法 JSONL
            if (business_jsonl_ && !esnap.detections.empty()) {
              std::string j;
              j.reserve(512);
              j += "{\"stream_id\":\"" + escape_json_str(cfg.stream_id) + "\"";
              j += ",\"frame_sequence\":" + std::to_string(vf.sequence);
              j += ",\"timestamp_ms\":" + std::to_string(t3);
              j += ",\"coordinate_mode\":\"" + escape_json_str(cfg.coordinate_mode) + "\"";
              j += ",\"enrichment_status\":\"" + std::string(enrichment_status_str(esnap.enrichment_status)) + "\"";
              j += ",\"detections\":[";
              for (size_t i = 0; i < esnap.detections.size(); ++i) {
                const auto& e = esnap.detections[i];
                if (i > 0) j += ",";
                char buf[1024];
                std::snprintf(buf, sizeof(buf),
                    "{\"detection_id\":%d,\"x1\":%.1f,\"y1\":%.1f,\"x2\":%.1f,\"y2\":%.1f,"
                    "\"score\":%.3f,\"longitude\":%.6f,\"latitude\":%.6f,"
                    "\"coordinate_valid\":%s,\"ais_matched\":%s,"
                    "\"speed\":%.2f,\"course\":%.2f,\"ais_distance_m\":%.2f,"
                    "\"ais_age_ms\":%lld,\"match_score\":%.4f,\"extrapolated\":%s,"
                    "\"enrichment_status\":\"%s\"",
                    e.detection_id, e.x1, e.y1, e.x2, e.y2, e.score,
                    e.longitude, e.latitude,
                    e.coordinate_valid ? "true" : "false",
                    e.ais_matched ? "true" : "false",
                    e.speed, e.course, e.ais_distance_m,
                    static_cast<long long>(e.ais_age_ms), e.match_score,
                    e.extrapolated ? "true" : "false",
                    enrichment_status_str(e.enrichment_status));
                j += buf;
                j += ",\"mmsi\":\"" + escape_json_str(e.mmsi) + "\"";
                j += ",\"ship_name\":\"" + escape_json_str(e.ship_name) + "\"";
                j += ",\"reject_reason\":\"" + escape_json_str(e.reject_reason) + "\"";
                if (e.ais_matched) {
                  std::snprintf(buf, sizeof(buf),
                      ",\"ais_lon\":%.6f,\"ais_lat\":%.6f,"
                      "\"ais_lon_aligned\":%.6f,\"ais_lat_aligned\":%.6f",
                      e.ais_lon, e.ais_lat, e.ais_lon_aligned, e.ais_lat_aligned);
                  j += buf;
                }
                j += "}";
              }
              j += "]}\n";
              std::fwrite(j.data(), 1, j.size(), business_jsonl_);
              std::fflush(business_jsonl_);
            }
            (void)enrich_ok;
          } else {
            // 未启用业务增强：仅检测
            for (const auto& d : snap.detections) {
              esnap.detections.emplace_back(d);
            }
          }
          enriched_snapshot_.update(esnap);
        } else {
          set_error(7, "推理失败: " + detector_->last_error());
        }
      }
    }

    // 绘框（使用融合快照，区分 AIS 匹配状态着色）
    // 绿色：AIS 已匹配；黄色：坐标有效但 AIS 未匹配；红色：业务增强不可用
    EnrichedDetectionSnapshot esnap = enriched_snapshot_.get();
    const bool has_draw = !esnap.expired(proc_start, cfg.result_ttl_ms) && !esnap.detections.empty();
    if (cpu_draw) {
      int64_t tdraw = now_ms();
      const uint8_t* src[2] = {f->data[0], f->data[1]};
      const int src_stride[2] = {f->linesize[0], f->linesize[1]};
      uint8_t* dst960[1] = {rgb.data.data()};
      const int dst_stride960[1] = {source_w_ * 3};
      sws_scale(nv12_to_rgb, src, src_stride, 0, source_h_, dst960, dst_stride960);
      if (has_draw) {
        for (const auto& e : esnap.detections) {
          Color c{0, 255, 0};  // 默认绿色（AIS 匹配）
          if (e.enrichment_status == EnrichmentState::COORD_ONLY) {
            c = {255, 255, 0};  // 黄色
          } else if (e.enrichment_status == EnrichmentState::DETECTION_ONLY) {
            c = {255, 0, 0};  // 红色
          }
          draw_rectangle(rgb, static_cast<int>(e.x1), static_cast<int>(e.y1),
                         static_cast<int>(e.x2), static_cast<int>(e.y2), c, 2);
          char label[64];
          if (e.ais_matched && !e.mmsi.empty()) {
            std::snprintf(label, sizeof(label), "MMSI:%s S:%.1f %.2f",
                          e.mmsi.c_str(), e.speed, e.score);
          } else {
            std::snprintf(label, sizeof(label), "ship %.2f", e.score);
          }
          draw_label(rgb, static_cast<int>(e.x1), static_cast<int>(e.y1), label, c, 2);
        }
      }
      const uint8_t* s2[1] = {rgb.data.data()};
      const int s2_stride[1] = {source_w_ * 3};
      uint8_t* d2[2] = {f->data[0], f->data[1]};
      const int d2_stride[2] = {f->linesize[0], f->linesize[1]};
      sws_scale(rgb_to_nv12, s2, s2_stride, 0, source_h_, d2, d2_stride);
      metrics_.record_draw_ms(ms_between(tdraw, now_ms()));
    } else if (bmcv_draw) {
      int64_t tdraw = now_ms();
      if (has_draw) {
        std::vector<BmcvProcessor::ColoredRect> rects;
        rects.reserve(esnap.detections.size());
        for (const auto& e : esnap.detections) {
          BmcvProcessor::ColoredRect r;
          r.x1 = e.x1; r.y1 = e.y1; r.x2 = e.x2; r.y2 = e.y2;
          if (e.enrichment_status == EnrichmentState::FULL) {
            r.r = 0; r.g = 255; r.b = 0;       // 绿色：AIS 匹配
          } else if (e.enrichment_status == EnrichmentState::COORD_ONLY) {
            r.r = 255; r.g = 255; r.b = 0;     // 黄色：坐标有效未匹配
          } else {
            r.r = 255; r.g = 0; r.b = 0;       // 红色：业务不可用
          }
          rects.push_back(r);
        }
        if (!bmcv_.draw_colored_rectangles(f, rects, berr)) {
          set_error(10, "BMCV 绘制失败: " + berr);
        }
        // 融合信息文字（BMCV 仅画矩形，文字在 host NV12 上补绘）
        for (const auto& e : esnap.detections) {
          char label[160];
          if (e.ais_matched && !e.mmsi.empty()) {
            if (!e.ship_name.empty()) {
              std::snprintf(label, sizeof(label), "MMSI:%s S:%.1f %s",
                            e.mmsi.c_str(), e.speed, e.ship_name.c_str());
            } else {
              std::snprintf(label, sizeof(label), "MMSI:%s S:%.1f",
                            e.mmsi.c_str(), e.speed);
            }
          } else {
            std::snprintf(label, sizeof(label), "ship %.2f", e.score);
          }
          draw_label_nv12(f->data[0], f->linesize[0],
                          f->data[1], f->linesize[1],
                          source_w_, source_h_,
                          static_cast<int>(e.x1), static_cast<int>(e.y1),
                          label, 2);
        }
      }
      metrics_.record_draw_ms(ms_between(tdraw, now_ms()));
    }

    const int64_t e2e = now_ms() - vf.capture_time_ms;
    metrics_.record_e2e_ms(static_cast<double>(e2e));
    metrics_.decode_queue_dropped.store(metrics_.decode_queue_dropped.load());
    metrics_.dropped_frames.store(q_decode_->dropped() + q_encode_->dropped());

    q_encode_->push(std::move(vf));
    vf.frame = nullptr;
  }

  if (nv12_to_rgb) sws_freeContext(nv12_to_rgb);
  if (nv12_to_rgb640) sws_freeContext(nv12_to_rgb640);
  if (rgb_to_nv12) sws_freeContext(rgb_to_nv12);
}

void SingleStreamPipeline::encode_loop(const PipelineConfig& cfg) {
  VideoFrame vf;
  std::string err;
  while (!stop_.load() && q_encode_->pop(vf)) {
    if (!vf.frame) continue;
    metrics_.encode_sent.fetch_add(1);
    int64_t t0 = now_ms();
    if (!sink_.write(vf, err)) {
      metrics_.rtmp_write_fail.fetch_add(1);
      set_error(8, "编码失败: " + err);
      vf.release();
      break;
    }
    double write_ms = ms_between(t0, now_ms());
    metrics_.record_encode_ms(write_ms);
    metrics_.record_rtmp_ms(write_ms);
    metrics_.output_frames.fetch_add(1);
    vf.release();
  }
}

void SingleStreamPipeline::metrics_loop(const PipelineConfig& cfg) {
  while (!stop_.load()) {
    for (int i = 0; i < cfg.metrics_interval_sec * 10 && !stop_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (stop_.load()) break;
    metrics_.set_queue_length(static_cast<int>(q_decode_->size() + q_encode_->size()));
    {
      std::lock_guard<std::mutex> lk(jitter_mutex_);
      metrics_.set_jitter_length(static_cast<int>(jitter_buf_.size()));
    }
    metrics_.rtsp_reconnects.store(source_.reconnect_count());
    metrics_.rtmp_reconnects.store(sink_.rtmp_reconnect_count());
    std::fprintf(stdout, "%s\n", metrics_.summary().c_str());
    std::fflush(stdout);
    if (cfg.max_seconds > 0 && (now_ms() - start_ms_) / 1000 >= cfg.max_seconds) {
      std::fprintf(stdout, "信息 | 限时 | 达到 %d 秒，主动停止\n", cfg.max_seconds);
      std::fflush(stdout);
      request_stop();
      break;
    }
  }
}

}  // namespace hzw
