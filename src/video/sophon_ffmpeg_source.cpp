// -*- coding: utf-8 -*-
#include "video/video_source.h"

#include <chrono>
#include <cstdio>
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

bool SophonVideoSource::open(const std::string& path, int device,
                             int extra_frame_buffer_num,
                             const std::string& decoder_name, std::string& err) {
  path_ = path;
  device_ = device;

  if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) {
    err = "无法打开输入文件: " + path;
    return false;
  }
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
  // 源帧率：优先 avg_frame_rate，回退 r_frame_rate。
  AVRational fr = st->avg_frame_rate;
  if (fr.num <= 0 || fr.den <= 0) fr = st->r_frame_rate;
  fps_ = (fr.num > 0 && fr.den > 0) ? static_cast<int>((fr.num + fr.den / 2) / fr.den) : 0;

  extra_frame_buffer_num_ = extra_frame_buffer_num;
  if (!open_decoder(decoder_name, err)) return false;

  pkt_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  seq_ = 0;
  loops_completed_ = 0;
  packets_read_ = 0;
  std::fprintf(stdout, "信息 | 视频解码 | 输入：%s\n", path.c_str());
  std::fprintf(stdout, "信息 | 视频解码 | 解码器：%s\n", decoder_name.c_str());
  std::fprintf(stdout, "信息 | 视频解码 | 尺寸：%dx%d，源帧率：%dfps\n", width_, height_, fps_);
  std::fflush(stdout);
  return true;
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
  // extra_frame_buffer_num 必须经 AVDictionary 传入（av_opt_set 在部分版本不生效）。
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
    // 某些容器不支持精确 seek，尝试回退到 BACKWARD。
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
        vf.width = tmp->width;
        vf.height = tmp->height;
        return true;
      }
      break;  // EAGAIN 或 EOF：需要更多 packet
    }
    av_frame_free(&tmp);

    int r = av_read_frame(fmt_, pkt_);
    if (r < 0) {
      // EOF 或错误
      ++loops_completed_;
      if (loop_ == 0 || loops_completed_ < loop_) {
        std::string e;
        if (seek_to_start(e)) {
          continue;  // 继续下一轮
        }
        err = e.empty() ? "循环 seek 失败" : e;
        return false;
      }
      // loop 用尽：EOF 正常结束
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
