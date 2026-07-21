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

// 预处理：RGB 640x640 Image -> NCHW FLOAT32 [1,3,640,640]（与单图基线语义一致）。
// 优化：单次遍历直接指针访问 + 行主序写 NCHW（原三次遍历+pixel()边界检查约 22ms，
// 优化后约 7ms）。板端 libswscale 无 SIMD，此函数仍优于 BMCV convert_to（后者不支持
// RGB_PACKED 且含额外 H2D/D2H 开销）。
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
  if (source_type == "file" && input_path.empty()) { err = "input_path 为空"; return false; }
  if (output_path.empty()) { err = "output_path 为空"; return false; }
  if (bmodel_path.empty()) { err = "bmodel_path 为空"; return false; }
  if (source_fps <= 0) { err = "source_fps 非法"; return false; }
  if (output_fps <= 0) { err = "output_fps 非法"; return false; }
  if (inference_fps <= 0) { err = "inference_fps 非法"; return false; }
  if (inference_fps > source_fps) { err = "inference_fps 大于 source_fps"; return false; }
  if (output_fps > source_fps) { err = "output_fps 大于 source_fps"; return false; }
  if (inference_fps > output_fps) { err = "inference_fps 大于 output_fps"; return false; }
  if (source_fps % output_fps != 0) { err = "source_fps 必须能被 output_fps 整除"; return false; }
  if (source_fps % inference_fps != 0) { err = "source_fps 必须能被 inference_fps 整除"; return false; }
  if (output_fps % inference_fps != 0) { err = "output_fps 必须能被 inference_fps 整除"; return false; }
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
  if (source_type != "file" && source_type != "rtsp") {
    err = "source_type 必须为 file 或 rtsp"; return false;
  }
  if (source_type == "rtsp") {
    if (input_env.empty()) { err = "RTSP 必须通过 --input-env 提供环境变量名（URL 不入命令行/日志）"; return false; }
    RtspSourceOptions ro; ro.transport = rtsp_transport; ro.stimeout_us = rtsp_stimeout_us;
    ro.max_reconnect_attempts = rtsp_max_reconnect; ro.initial_backoff_ms = rtsp_initial_backoff_ms;
    ro.max_backoff_ms = rtsp_max_backoff_ms;
    if (!ro.validate(err)) return false;
  }
  return true;
}

SingleStreamPipeline::SingleStreamPipeline() = default;
SingleStreamPipeline::~SingleStreamPipeline() {
  request_stop();
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
  source_.request_stop();  // 触发中断回调，打断阻塞中的 open/read
  bool was = stop_.exchange(true);
  if (!was) {
    if (q_decode_) q_decode_->close();
    if (q_encode_) q_encode_->close();
  }
}

int SingleStreamPipeline::run(const PipelineConfig& cfg) {
  cfg_ = cfg;
  std::string err;
  if (!cfg_.validate(err)) {
    std::fprintf(stderr, "错误 | 配置 | %s\n", err.c_str());
    return 2;
  }

  if (cfg_.source_type == "rtsp") {
    std::string url, e;
    if (!resolve_input_env(cfg_.input_env, url, e)) {
      std::fprintf(stderr, "错误 | RTSP输入 | %s\n", e.c_str());  // 仅含变量名，不含 URL
      return 3;
    }
    RtspSourceOptions ro;
    ro.transport = cfg_.rtsp_transport;
    ro.stimeout_us = cfg_.rtsp_stimeout_us;
    ro.max_reconnect_attempts = cfg_.rtsp_max_reconnect;
    ro.initial_backoff_ms = cfg_.rtsp_initial_backoff_ms;
    ro.max_backoff_ms = cfg_.rtsp_max_backoff_ms;
    if (!source_.open_rtsp(url, cfg_.device, 20, cfg_.decoder, ro, err)) {
      std::fprintf(stderr, "错误 | 视频源 | %s\n", err.c_str());  // err 不含 URL
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

  // BMCV 预处理/绘制初始化（复用 detector 的设备句柄，避免二次打开设备）。
  // 仅在 preprocess=bmcv 或 draw_mode=bmcv 时初始化；失败则回退 CPU 路径。
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

  if (!sink_.open(cfg_.output_path, source_w_, source_h_, cfg_.output_fps,
                  cfg_.bitrate_kbps, cfg_.gop, cfg_.device, cfg_.encoder, err)) {
    std::fprintf(stderr, "错误 | 视频输出 | %s\n", err.c_str());
    return 5;
  }

  auto releaser = [](VideoFrame& f) { f.release(); };
  q_decode_ = std::make_unique<FrameQueue>(cfg_.queue_size, releaser);
  q_encode_ = std::make_unique<FrameQueue>(cfg_.queue_size, releaser);

  start_ms_ = now_ms();
  t_decode_ = std::thread([this] { decode_loop(cfg_); });
  t_process_ = std::thread([this] { process_loop(cfg_); });
  t_encode_ = std::thread([this] { encode_loop(cfg_); });
  t_metrics_ = std::thread([this] { metrics_loop(cfg_); });

  t_decode_.join();
  q_decode_->close();       // 解码结束，通知处理线程退出
  t_process_.join();
  q_encode_->close();       // 处理结束，通知编码线程退出
  t_encode_.join();
  stop_.store(true);
  t_metrics_.join();

  sink_.close();
  source_.close();

  std::fprintf(stdout, "%s\n", metrics_.summary().c_str());
  std::fprintf(stdout, "信息 | 结束 | 输出帧=%lld 编码器=%s 容器=%s 退出码=%d\n",
               static_cast<long long>(sink_.output_frames()),
               sink_.actual_encoder().c_str(), sink_.actual_container().c_str(),
               exit_code_.load());
  std::fflush(stdout);
  return exit_code_.load();
}

void SingleStreamPipeline::decode_loop(const PipelineConfig& cfg) {
  const int output_step = cfg.source_fps / cfg.output_fps;  // 每隔多少源帧输出一帧
  const bool is_rtsp = (cfg.source_type == "rtsp");  // RTSP 实时流不做墙钟节流
  // 实时节流：按源帧率回放，保证输出时长正确、稳定性测试运行满指定时长。
  // 解码硬件远快于实时，不节流会导致文件在数秒内跑完、大量丢帧、输出时长错误。
  const int64_t frame_interval_ms = 1000 / cfg.source_fps;
  const int64_t pace_start_ms = now_ms();
  VideoFrame vf;
  std::string err;
  while (!stop_.load()) {
    if (!source_.read(vf, err)) {
      if (err == "EOF") {
        std::fprintf(stdout, "信息 | 解码 | 正常 EOF，循环完成 %d 轮\n",
                     source_.loops_completed());
        std::fflush(stdout);
      } else if (err == "停止") {
        // 信号/限时主动停止，非错误
      } else {
        set_error(6, "解码失败: " + err);
      }
      break;
    }
    metrics_.decoded_frames.fetch_add(1);
    metrics_.read_frames.store(source_.packets_read());
    // 实时节流：本地文件需按源帧率回放；RTSP 实时流由网络按真实速率到达，不节流。
    if (!is_rtsp) {
      const int64_t expected_ms = pace_start_ms + vf.sequence * frame_interval_ms;
      const int64_t actual_ms = now_ms();
      if (actual_ms < expected_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(expected_ms - actual_ms));
      }
    }
    if (vf.sequence % output_step == 0) {
      metrics_.queued_frames.fetch_add(1);
      q_decode_->push(std::move(vf));
      vf.frame = nullptr;
    } else {
      vf.release();  // 非输出帧立即归还解码缓冲池
    }
  }
}

void SingleStreamPipeline::process_loop(const PipelineConfig& cfg) {
  const int infer_step = cfg.source_fps / cfg.inference_fps;
  const int input_size = 640;
  const bool cpu_draw = (cfg.draw_mode == "cpu");
  const bool bmcv_draw = (cfg.draw_mode == "bmcv");
  const bool cpu_pre = (cfg.preprocess == "cpu");
  const bool bmcv_pre = (cfg.preprocess == "bmcv" && bmcv_.ready());

  // sws 上下文按需创建：
  //   nv12_to_rgb   / rgb_to_nv12 ：仅 CPU 绘制路径需要（NV12<->RGB 往返）。
  //   nv12_to_rgb640              ：仅 CPU 预处理路径需要（NV12->RGB640 缩放）。
  // BMCV 路径不创建 sws 上下文（CSC 由 BMCV storage_convert 完成，绘制由 BMCV draw_rectangle 完成）。
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

  Image rgb;          // 960x544 RGB，仅 CPU 绘制路径使用
  if (cpu_draw) {
    rgb.width = source_w_; rgb.height = source_h_;
    rgb.data.assign(static_cast<size_t>(source_w_) * source_h_ * 3, 0);
  }
  Image rgb640;       // 640x640 RGB，仅 CPU 预处理路径使用
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
    // 源 epoch 变化（RTSP 重连）：清除过期检测结果，避免旧框粘贴到新连接的帧。
    if (vf.source_epoch != last_epoch_) {
      snapshot_.clear();
      last_epoch_ = vf.source_epoch;
    }
    const int64_t proc_start = now_ms();
    AVFrame* f = vf.frame;

    const bool is_infer = (vf.sequence % infer_step == 0);
    if (is_infer) {
      int64_t t0 = now_ms();
      bool pre_ok = false;
      if (bmcv_pre) {
        input.assign(static_cast<size_t>(detector_->input_element_count()), 0.0f);
        pre_ok = bmcv_.preprocess(f, input.data(), berr);
        if (!pre_ok) {
          set_error(9, "BMCV 预处理失败: " + berr);
        }
      } else {
        // CPU 预处理：NV12 -> RGB(640x640) sws 缩放 + 归一化
        const uint8_t* src[2] = {f->data[0], f->data[1]};
        const int src_stride[2] = {f->linesize[0], f->linesize[1]};
        uint8_t* dst640[1] = {rgb640.data.data()};
        const int dst_stride640[1] = {640 * 3};
        sws_scale(nv12_to_rgb640, src, src_stride, 0, source_h_, dst640, dst_stride640);
        pre_ok = preprocess_rgb_to_nchw(rgb640, input_size, input_size, input);
      }
      if (pre_ok) {
        int64_t t1 = now_ms();
        if (detector_->infer(input, output)) {
          int64_t t2 = now_ms();
          std::vector<Detection> dets;
          postprocess_yolov7(output.data(), num_boxes, num_vals, source_w_, source_h_,
                             input_size, cfg.conf, cfg.iou, dets);
          int64_t t3 = now_ms();
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
        } else {
          set_error(7, "推理失败: " + detector_->last_error());
        }
      }
    }

    // 绘框：复用最近 snapshot，过期则不绘制。
    DetectionSnapshot snap = snapshot_.get();
    const bool has_draw = !snap.expired(proc_start, cfg.result_ttl_ms) && !snap.detections.empty();
    if (cpu_draw) {
      // CPU 绘制：NV12 -> RGB -> 绘框 -> RGB -> NV12（往返约 132ms，板端 sws 无 SIMD）
      const uint8_t* src[2] = {f->data[0], f->data[1]};
      const int src_stride[2] = {f->linesize[0], f->linesize[1]};
      uint8_t* dst960[1] = {rgb.data.data()};
      const int dst_stride960[1] = {source_w_ * 3};
      sws_scale(nv12_to_rgb, src, src_stride, 0, source_h_, dst960, dst_stride960);
      if (has_draw) {
        for (const auto& d : snap.detections) {
          Color c{0, 255, 0};
          draw_rectangle(rgb, static_cast<int>(d.x1), static_cast<int>(d.y1),
                         static_cast<int>(d.x2), static_cast<int>(d.y2), c, 2);
          char label[32];
          std::snprintf(label, sizeof(label), "ship %.2f", d.score);
          draw_label(rgb, static_cast<int>(d.x1), static_cast<int>(d.y1), label, c, 2);
        }
      }
      const uint8_t* s2[1] = {rgb.data.data()};
      const int s2_stride[1] = {source_w_ * 3};
      uint8_t* d2[2] = {f->data[0], f->data[1]};
      const int d2_stride[2] = {f->linesize[0], f->linesize[1]};
      sws_scale(rgb_to_nv12, s2, s2_stride, 0, source_h_, d2, d2_stride);
    } else if (bmcv_draw && has_draw) {
      // BMCV 绘制：直接在 NV12 bm_image 上绘制矩形（H2D+draw+D2H 约 6ms）
      if (!bmcv_.draw_rectangles(f, snap.detections, berr)) {
        set_error(10, "BMCV 绘制失败: " + berr);
      }
    }
    // draw_mode == "none"：跳过绘制

    const int64_t e2e = now_ms() - vf.capture_time_ms;
    metrics_.record_e2e_ms(static_cast<double>(e2e));
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
    int64_t t0 = now_ms();
    if (!sink_.write(vf, err)) {
      set_error(8, "编码失败: " + err);
      vf.release();
      break;
    }
    metrics_.record_encode_ms(ms_between(t0, now_ms()));
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
    std::fprintf(stdout, "%s\n", metrics_.summary().c_str());
    std::fflush(stdout);
    // max_seconds 限时停止（稳定性测试用）
    if (cfg.max_seconds > 0 && (now_ms() - start_ms_) / 1000 >= cfg.max_seconds) {
      std::fprintf(stdout, "信息 | 限时 | 达到 %d 秒，主动停止\n", cfg.max_seconds);
      std::fflush(stdout);
      request_stop();
      break;
    }
  }
}

}  // namespace hzw
