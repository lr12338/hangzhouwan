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
  is_rtsp_ = false;

  if (!open_input(path, nullptr, err)) return false;
  if (!open_decoder(decoder_name, err)) return false;

  pkt_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  seq_ = 0;
  loops_completed_ = 0;
  packets_read_ = 0;
  source_epoch_ = 1;
  std::fprintf(stdout, "信息 | 视频解码 | 输入：%s\n", path.c_str());
  std::fprintf(stdout, "信息 | 视频解码 | 解码器：%s\n", decoder_name.c_str());
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
  if (!open_decoder(decoder_name, err)) return false;

  pkt_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  seq_ = 0;
  loops_completed_ = 0;
  packets_read_ = 0;
  reconnect_count_ = 0;
  source_epoch_ = 1;
  std::fprintf(stdout, "信息 | 视频解码 | 输入(RTSP)：%s\n", redact_url_credentials(url).c_str());
  std::fprintf(stdout, "信息 | 视频解码 | 解码器：%s 传输：%s stimeout=%lldus\n",
               decoder_name.c_str(), opts.transport.c_str(),
               static_cast<long long>(opts.stimeout_us));
  std::fprintf(stdout, "信息 | 视频解码 | 尺寸：%dx%d，源帧率：%dfps\n", width_, height_, fps_);
  std::fflush(stdout);
  return true;
}

bool SophonVideoSource::try_reopen_rtsp(std::string& err) {
  // 仅关闭输入与解码器，保留 pkt_/frame_ 复用。
  close_decoder();
  if (fmt_) avformat_close_input(&fmt_);
  video_index_ = -1;

  AVDictionary* dict = nullptr;
  av_dict_set(&dict, "rtsp_transport", rtsp_opts_.transport.c_str(), 0);
  if (rtsp_opts_.stimeout_us > 0)
    av_dict_set_int(&dict, "stimeout", rtsp_opts_.stimeout_us, 0);
  bool ok = open_input(rtsp_url_, &dict, err);
  av_dict_free(&dict);
  if (!ok) return false;
  // 解码器名固定 h264_bm，从原 decoder 不可达；这里沿用 h264_bm。
  if (!open_decoder("h264_bm", err)) return false;
  return true;
}

bool SophonVideoSource::reconnect(std::string& err) {
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
    std::string e;
    if (try_reopen_rtsp(e)) {
      reconnect_policy_.on_success();
      ++reconnect_count_;
      ++source_epoch_;
      std::fprintf(stdout, "信息 | RTSP重连 | 重连成功（第%d次，epoch=%d）\n",
                   reconnect_count_, source_epoch_);
      std::fflush(stdout);
      return true;
    }
    // 重连失败：记录原因（不含 URL），继续退避循环
    std::fprintf(stdout, "信息 | RTSP重连 | 重连失败：%s\n", e.c_str());
    std::fflush(stdout);
  }
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
    err = std::string("打开解码器失败: ") + eb;
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
  while (true) {
    // 先消费已发送 packet 产生的帧。
    AVFrame* tmp = av_frame_alloc();
    while (true) {
      int rr = avcodec_receive_frame(dec_, tmp);
      if (rr == 0) {
        vf.frame = tmp;  // 所有权转移
        vf.sequence = seq_++;
        vf.pts = (tmp->pts != AV_NOPTS_VALUE) ? tmp->pts : 0;
        vf.capture_time_ms = now_ms();
        vf.source_epoch = source_epoch_;
        vf.width = tmp->width;
        vf.height = tmp->height;
        return true;
      }
      break;  // EAGAIN 或 EOF：需要更多 packet
    }
    av_frame_free(&tmp);

    int r = av_read_frame(fmt_, pkt_);
    if (r < 0) {
      if (is_rtsp_) {
        // RTSP 断流/读取超时：受控重连（除非已请求停止）
        if (stop_requested_.load()) {
          err = "停止";
          return false;
        }
        std::fprintf(stdout, "信息 | RTSP重连 | 读取失败/断流，进入重连流程\n");
        std::fflush(stdout);
        if (!reconnect(err)) return false;
        continue;  // 在新连接上继续读取
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
      avcodec_send_packet(dec_, pkt_);
    }
    av_packet_unref(pkt_);
  }
}

void SophonVideoSource::close_decoder() {
  if (dec_) {
    avcodec_free_context(&dec_);
  }
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
