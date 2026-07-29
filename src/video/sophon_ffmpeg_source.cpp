// -*- coding: utf-8 -*-
#include "video/video_source.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include "util/sha256.h"  // 保持与现有模块一致的头文件包含风格

namespace hzw {

namespace {
int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
bool is_enomem(int averr) { return averr == AVERROR(ENOMEM); }
}  // namespace

SophonVideoSource::~SophonVideoSource() { close(); }

int SophonVideoSource::interrupt_cb(void* opaque) {
  auto* self = static_cast<SophonVideoSource*>(opaque);
  return (self && self->stop_requested_.load()) ? 1 : 0;
}

void SophonVideoSource::sleep_interruptible(int64_t ms) {
  const int64_t step = 50;
  for (int64_t slept = 0; slept < ms; slept += step) {
    if (stop_requested_.load()) return;
    int64_t chunk = (ms - slept < step) ? (ms - slept) : step;
    std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
  }
}

void SophonVideoSource::mark_resource_fatal(const std::string& reason) {
  resource_fatal_.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(fatal_mutex_);
    fatal_reason_ = reason;
  }
  std::fprintf(stderr, "致命 | 视频源 | DEVICE_RESOURCE_FATAL: %s\n", reason.c_str());
  std::fflush(stderr);
}

std::string SophonVideoSource::fatal_reason() const {
  std::lock_guard<std::mutex> lk(fatal_mutex_);
  return fatal_reason_;
}

bool SophonVideoSource::open_input(const std::string& url, AVDictionary** opts_ptr,
                                   std::string& err) {
  // 预分配 ctx 并设置中断回调，使 avformat_open_input/find_stream_info 可被停止信号中断。
  AVFormatContext* ctx = avformat_alloc_context();
  if (!ctx) {
    err = "分配格式上下文失败";
    return false;
  }
  ctx->interrupt_callback.callback = &SophonVideoSource::interrupt_cb;
  ctx->interrupt_callback.opaque = this;
  int r = avformat_open_input(&ctx, url.c_str(), nullptr, opts_ptr);
  if (r < 0 || !ctx) {
    char eb[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(r < 0 ? r : AVERROR_UNKNOWN, eb, sizeof(eb));
    // ctx 失败时由 avformat_open_input 释放并置空，无需再释放。
    err = std::string("打开输入失败: ") + eb;
    return false;
  }
  fmt_ = ctx;
  if (avformat_find_stream_info(fmt_, nullptr) < 0) {
    err = "无法获取流信息";
    avformat_close_input(&fmt_);
    return false;
  }
  for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
    if (fmt_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_index_ = static_cast<int>(i);
      break;
    }
  }
  if (video_index_ < 0) {
    err = "未找到视频流";
    avformat_close_input(&fmt_);
    return false;
  }
  AVStream* st = fmt_->streams[video_index_];
  width_ = st->codecpar->width;
  height_ = st->codecpar->height;
  time_base_ = st->time_base;
  AVRational fr = st->avg_frame_rate;
  if (fr.num <= 0 || fr.den <= 0) fr = st->r_frame_rate;
  fps_ = (fr.num > 0 && fr.den > 0) ? static_cast<int>((fr.num + fr.den / 2) / fr.den) : 0;
  return true;
}

bool SophonVideoSource::open(const std::string& path, int device,
                             int extra_frame_buffer_num,
                             const std::string& decoder_name, std::string& err) {
  path_ = path;
  device_ = device;
  extra_frame_buffer_num_ = extra_frame_buffer_num;
  decoder_name_ = decoder_name;
  is_rtsp_ = false;

  if (!open_input(path, nullptr, err)) return false;
  if (!open_decoder(decoder_name, err)) {
    avformat_close_input(&fmt_);
    return false;
  }

  pkt_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  seq_ = 0;
  loops_completed_ = 0;
  packets_read_ = 0;
  source_epoch_ = 1;
  std::fprintf(stdout, "信息 | 视频解码 | 输入：%s\n", path.c_str());
  std::fprintf(stdout, "信息 | 视频解码 | 解码器：%s extra_frame_buffer_num=%d\n",
               decoder_name.c_str(), extra_frame_buffer_num_);
  std::fprintf(stdout, "信息 | 视频解码 | 尺寸：%dx%d，源帧率：%dfps\n", width_, height_, fps_);
  std::fflush(stdout);
  return true;
}

bool SophonVideoSource::open_rtsp(const std::string& url, int device,
                                  int extra_frame_buffer_num,
                                  const std::string& decoder_name,
                                  const RtspSourceOptions& opts, std::string& err) {
  if (!opts.validate(err)) return false;
  rtsp_url_ = url;
  device_ = device;
  extra_frame_buffer_num_ = extra_frame_buffer_num;
  decoder_name_ = decoder_name;
  is_rtsp_ = true;
  rtsp_opts_ = opts;
  reconnect_policy_ = ReconnectPolicy(opts.initial_backoff_ms, opts.max_backoff_ms);

  AVDictionary* dict = nullptr;
  av_dict_set(&dict, "rtsp_transport", opts.transport.c_str(), 0);
  if (opts.stimeout_us > 0)
    av_dict_set_int(&dict, "stimeout", opts.stimeout_us, 0);
  bool ok = open_input(url, &dict, err);
  av_dict_free(&dict);
  if (!ok) return false;
  if (!open_decoder(decoder_name, err)) {
    avformat_close_input(&fmt_);
    return false;
  }

  pkt_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  seq_ = 0;
  loops_completed_ = 0;
  packets_read_ = 0;
  reconnect_count_ = 0;
  reconnect_attempt_count_.store(0);
  reconnect_failure_count_.store(0);
  source_epoch_ = 1;
  std::fprintf(stdout, "信息 | 视频解码 | 输入(RTSP)：%s\n", redact_url_credentials(url).c_str());
  std::fprintf(stdout, "信息 | 视频解码 | 解码器：%s 传输：%s stimeout=%lldus extra_frame_buffer_num=%d\n",
               decoder_name.c_str(), opts.transport.c_str(),
               static_cast<long long>(opts.stimeout_us), extra_frame_buffer_num_);
  std::fprintf(stdout, "信息 | 视频解码 | 尺寸：%dx%d，源帧率：%dfps\n", width_, height_, fps_);
  std::fflush(stdout);
  return true;
}

bool SophonVideoSource::try_reopen(const std::string& url, const std::string& decoder_name,
                                   AVDictionary** opts_ptr, std::string& err) {
  // 前置：在途帧必须已归零。关闭旧解码器（受在途帧保护）。
  close_decoder();
  if (resource_fatal_.load()) {
    err = "DEVICE_RESOURCE_FATAL: 旧解码器在途帧未归零，拒绝换建";
    return false;
  }
  if (fmt_) avformat_close_input(&fmt_);
  video_index_ = -1;

  if (!open_input(url, opts_ptr, err)) return false;  // open_input 失败已自清理 fmt_
  if (!open_decoder(decoder_name, err)) {
    // open_decoder 失败已清理 dec_；关闭已打开的输入，避免半初始化上下文残留。
    if (fmt_) avformat_close_input(&fmt_);
    return false;
  }
  return true;
}

bool SophonVideoSource::reconnect_rtsp(std::string& err) {
  // 调用方（管线）已停止产生新帧、清空 jitter/decode/encode 队列并等待在途帧归零。
  while (true) {
    if (stop_requested_.load()) {
      err = "停止";
      return false;
    }
    if (rtsp_opts_.max_reconnect_attempts >= 0 &&
        reconnect_policy_.attempts() >= rtsp_opts_.max_reconnect_attempts) {
      err = "RTSP 重连次数耗尽（上限 " +
            std::to_string(rtsp_opts_.max_reconnect_attempts) + "）";
      return false;
    }
    int64_t backoff = reconnect_policy_.on_failure();
    std::fprintf(stdout,
                 "信息 | RTSP重连 | BACKOFF 第%d次 退避%lldms（URL已脱敏）\n",
                 reconnect_policy_.attempts(), static_cast<long long>(backoff));
    std::fflush(stdout);
    sleep_interruptible(backoff);
    if (stop_requested_.load()) {
      err = "停止";
      return false;
    }
    reconnect_attempt_count_.fetch_add(1);
    AVDictionary* dict = nullptr;
    av_dict_set(&dict, "rtsp_transport", rtsp_opts_.transport.c_str(), 0);
    if (rtsp_opts_.stimeout_us > 0)
      av_dict_set_int(&dict, "stimeout", rtsp_opts_.stimeout_us, 0);
    std::string e;
    bool ok = try_reopen(rtsp_url_, "h264_bm", &dict, e);
    av_dict_free(&dict);
    if (ok) {
      reconnect_policy_.on_success();
      ++reconnect_count_;
      ++source_epoch_;
      std::fprintf(stdout, "信息 | RTSP重连 | 重连成功（第%d次，epoch=%d）\n",
                   reconnect_count_, source_epoch_);
      std::fflush(stdout);
      return true;
    }
    reconnect_failure_count_.fetch_add(1);
    // 资源致命（VPU/解码器 ENOMEM）立即升级，禁止错误风暴式重试。
    if (resource_fatal_.load()) {
      err = e;
      return false;
    }
    std::fprintf(stdout, "信息 | RTSP重连 | 重连失败：%s\n", e.c_str());
    std::fflush(stdout);
  }
}

bool SophonVideoSource::simulate_reconnect(std::string& err) {
  // 测试钩子：复用与生产重连等价的“等待在途帧归零 -> 换建解码器”路径。
  if (!wait_avframes_drained(5000)) {
    mark_resource_fatal("simulate_reconnect 在途帧未归零");
    err = "DEVICE_RESOURCE_FATAL: 在途帧未归零";
    return false;
  }
  if (is_rtsp_) {
    AVDictionary* dict = nullptr;
    av_dict_set(&dict, "rtsp_transport", rtsp_opts_.transport.c_str(), 0);
    if (rtsp_opts_.stimeout_us > 0)
      av_dict_set_int(&dict, "stimeout", rtsp_opts_.stimeout_us, 0);
    bool ok = try_reopen(rtsp_url_, "h264_bm", &dict, err);
    av_dict_free(&dict);
    if (ok) { ++reconnect_count_; ++source_epoch_; }
    return ok;
  }
  bool ok = try_reopen(path_, decoder_name_, nullptr, err);
  if (ok) { ++reconnect_count_; ++source_epoch_; }
  return ok;
}

bool SophonVideoSource::open_decoder(const std::string& decoder_name, std::string& err) {
  const AVCodec* dec = avcodec_find_decoder_by_name(decoder_name.c_str());
  if (!dec) {
    err = "找不到解码器: " + decoder_name;
    return false;
  }
  dec_ = avcodec_alloc_context3(dec);
  if (!dec_) {
    err = "分配解码器上下文失败";
    return false;
  }
  if (avcodec_parameters_to_context(dec_, fmt_->streams[video_index_]->codecpar) < 0) {
    err = "复制解码参数失败";
    avcodec_free_context(&dec_);  // 立即清理半初始化上下文
    return false;
  }
  av_opt_set_int(dec_, "sophon_idx", device_, 0);
  AVDictionary* opts = nullptr;
  av_dict_set_int(&opts, "extra_frame_buffer_num", extra_frame_buffer_num_, 0);
  int r = avcodec_open2(dec_, dec, &opts);
  av_dict_free(&opts);
  if (r < 0) {
    char eb[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(r, eb, sizeof(eb));
    avcodec_free_context(&dec_);  // 立即清理半初始化上下文
    if (is_enomem(r)) {
      mark_resource_fatal(std::string("解码器打开内存不足: ") + eb);
      err = std::string("DEVICE_RESOURCE_FATAL: 解码器打开内存不足: ") + eb;
    } else {
      err = std::string("打开解码器失败: ") + eb;
    }
    return false;
  }
  return true;
}

bool SophonVideoSource::seek_to_start(std::string& err) {
  avcodec_flush_buffers(dec_);
  if (avformat_seek_file(fmt_, video_index_, INT64_MIN, 0, INT64_MAX, 0) < 0) {
    if (av_seek_frame(fmt_, video_index_, 0, AVSEEK_FLAG_BACKWARD) < 0) {
      err = "循环 seek 失败";
      return false;
    }
  }
  return true;
}

bool SophonVideoSource::read(VideoFrame& vf, std::string& err) {
  if (!dec_) {
    err = "解码器未打开";
    return false;
  }
  if (resource_fatal_.load()) {
    err = "DEVICE_RESOURCE_FATAL: " + fatal_reason();
    return false;
  }
  while (true) {
    // 先消费已发送 packet 产生的帧。
    AVFrame* tmp = av_frame_alloc();
    if (!tmp) {
      mark_resource_fatal("av_frame_alloc 失败");
      err = "DEVICE_RESOURCE_FATAL: av_frame_alloc 失败";
      return false;
    }
    int rr = avcodec_receive_frame(dec_, tmp);
    if (rr == 0) {
      vf.frame = tmp;                       // 所有权转移
      vf.frame_tracker_ = &inflight_;       // 绑定在途计数器
      inflight_.on_produce();
      vf.sequence = seq_++;
      vf.pts = (tmp->pts != AV_NOPTS_VALUE) ? tmp->pts : 0;
      vf.capture_time_ms = now_ms();
      vf.source_epoch = source_epoch_;
      vf.width = tmp->width;
      vf.height = tmp->height;
      return true;
    }
    av_frame_free(&tmp);
    if (rr != AVERROR(EAGAIN)) {
      char eb[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(rr, eb, sizeof(eb));
      if (is_enomem(rr)) {
        mark_resource_fatal(std::string("解码器内存不足(receive_frame): ") + eb);
        err = std::string("DEVICE_RESOURCE_FATAL: ") + eb;
        return false;
      }
      if (rr != AVERROR_EOF) {
        // 单个坏包不应致命：记录并继续读取下一包。
        std::fprintf(stdout, "警告 | 视频解码 | receive_frame 错误：%s，跳过\n", eb);
        std::fflush(stdout);
      }
    }

    int r = av_read_frame(fmt_, pkt_);
    if (r < 0) {
      if (is_rtsp_) {
        // RTSP 断流/读取超时：交由管线协调重连（先清空缓冲、等待在途帧归零）。
        if (stop_requested_.load()) {
          err = "停止";
          return false;
        }
        std::fprintf(stdout, "信息 | RTSP重连 | 读取失败/断流，交由管线协调重连\n");
        std::fflush(stdout);
        err = "RTSP_RECONNECT";
        return false;
      }
      // 本地文件：EOF 或错误
      ++loops_completed_;
      if (loop_ == 0 || loops_completed_ < loop_) {
        std::string e;
        if (seek_to_start(e)) {
          continue;
        }
        err = e.empty() ? "循环 seek 失败" : e;
        return false;
      }
      err = "EOF";
      return false;
    }
    ++packets_read_;
    if (pkt_->stream_index == video_index_) {
      int sp = avcodec_send_packet(dec_, pkt_);
      if (sp < 0 && sp != AVERROR(EAGAIN)) {
        char eb[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(sp, eb, sizeof(eb));
        if (is_enomem(sp)) {
          mark_resource_fatal(std::string("解码器内存不足(send_packet): ") + eb);
          err = std::string("DEVICE_RESOURCE_FATAL: ") + eb;
          av_packet_unref(pkt_);
          return false;
        }
        if (sp != AVERROR_EOF) {
          std::fprintf(stdout, "警告 | 视频解码 | send_packet 错误：%s，跳过该包\n", eb);
          std::fflush(stdout);
        }
      }
    }
    av_packet_unref(pkt_);
  }
}

void SophonVideoSource::close_decoder() {
  if (!dec_) return;
  // 硬性保护：在途帧引用解码器 bm_image 池时禁止关闭，否则设备内存泄漏。
  if (inflight_.count() != 0) {
    mark_resource_fatal("关闭解码器时仍有 " + std::to_string(inflight_.count()) +
                        " 帧在途，拒绝关闭以防 VPU 显存泄漏");
    return;  // 不释放 dec_，交由进程退出回收
  }
  avcodec_free_context(&dec_);
}

void SophonVideoSource::close() {
  if (pkt_) {
    av_packet_free(&pkt_);
  }
  if (frame_) {
    av_frame_free(&frame_);
  }
  close_decoder();
  if (fmt_) {
    avformat_close_input(&fmt_);
  }
}

}  // namespace hzw
