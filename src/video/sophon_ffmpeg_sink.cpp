// -*- coding: utf-8 -*-
#include "video/video_sink.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>

namespace hzw {

SophonVideoSink::~SophonVideoSink() { close(); }

bool SophonVideoSink::open(const std::string& path, int width, int height, int fps,
                           int bitrate_kbps, int gop, int device,
                           const std::string& encoder_name, std::string& err) {
  if (!open_encoder(width, height, fps, bitrate_kbps, gop, device, encoder_name, err)) {
    return false;
  }
  pkt_ = av_packet_alloc();
  if (!open_muxer(path, err)) {
    return false;
  }
  std::fprintf(stdout, "信息 | 视频编码 | 编码器：%s 容器：%s %dx%d %dfps %dkbps gop=%d\n",
               encoder_name_.c_str(), container_.c_str(), width, height, fps,
               bitrate_kbps, gop);
  std::fflush(stdout);
  return true;
}

bool SophonVideoSink::open_encoder(int width, int height, int fps, int bitrate_kbps,
                                   int gop, int device,
                                   const std::string& encoder_name, std::string& err) {
  const AVCodec* enc = avcodec_find_encoder_by_name(encoder_name.c_str());
  if (!enc) {
    err = "找不到编码器: " + encoder_name;
    return false;
  }
  enc_ = avcodec_alloc_context3(enc);
  if (!enc_) {
    err = "分配编码器上下文失败";
    return false;
  }
  enc_->width = width;
  enc_->height = height;
  enc_->time_base = (AVRational){1, fps};
  enc_->framerate = (AVRational){fps, 1};
  enc_->pix_fmt = AV_PIX_FMT_NV12;
  enc_->bit_rate = static_cast<int64_t>(bitrate_kbps) * 1000;
  enc_->gop_size = gop;
  enc_->max_b_frames = 0;  // 低延迟 IPPP
  av_opt_set_int(enc_, "sophon_idx", device, 0);
  // 输入为解码帧 bm_image（DMA 缓冲），is_dma_buffer 保持默认 1。
  int r = avcodec_open2(enc_, enc, nullptr);
  if (r < 0) {
    char eb[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(r, eb, sizeof(eb));
    err = std::string("打开编码器失败: ") + eb;
    return false;
  }
  encoder_name_ = enc->name;  // 实际编码器名
  return true;
}

bool SophonVideoSink::open_muxer(const std::string& path, std::string& err) {
  // 依据扩展名选择容器；MP4 失败则回退到 TS。
  std::string ext;
  auto pos = path.find_last_of('.');
  if (pos != std::string::npos) ext = path.substr(pos + 1);
  // 转小写
  for (auto& c : ext) c = static_cast<char>(std::tolower(c));

  const char* mux_fmt = nullptr;
  if (ext == "mp4" || ext == "mov") mux_fmt = "mp4";
  else if (ext == "ts" || ext == "m2ts") mux_fmt = "mpegts";
  else if (ext == "h264" || ext == "264") mux_fmt = "h264";
  else mux_fmt = "mp4";  // 默认尝试 mp4

  auto try_open = [&](const char* fmt, const std::string& out_path,
                      std::string& e) -> bool {
    AVFormatContext* m = nullptr;
    if (avformat_alloc_output_context2(&m, nullptr, fmt, out_path.c_str()) < 0 || !m) {
      e = "分配输出封装失败";
      return false;
    }
    mux_ = m;
    vstream_ = avformat_new_stream(mux_, nullptr);
    if (!vstream_) {
      e = "新建输出流失败";
      avformat_free_context(mux_);
      mux_ = nullptr;
      return false;
    }
    vstream_->time_base = enc_->time_base;
    if (avcodec_parameters_from_context(vstream_->codecpar, enc_) < 0) {
      e = "复制编码参数失败";
      avformat_free_context(mux_);
      mux_ = nullptr;
      return false;
    }
    if (!(mux_->oformat->flags & AVFMT_NOFILE)) {
      if (avio_open(&mux_->pb, out_path.c_str(), AVIO_FLAG_WRITE) < 0) {
        e = "打开输出文件失败: " + out_path;
        avformat_free_context(mux_);
        mux_ = nullptr;
        return false;
      }
    }
    AVDictionary* opts = nullptr;
    int r = avformat_write_header(mux_, &opts);
    av_dict_free(&opts);
    if (r < 0) {
      char eb[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(r, eb, sizeof(eb));
      e = std::string("写封装头失败: ") + eb;
      if (!(mux_->oformat->flags & AVFMT_NOFILE)) avio_closep(&mux_->pb);
      avformat_free_context(mux_);
      mux_ = nullptr;
      return false;
    }
    header_written_ = true;
    return true;
  };

  std::string e;
  if (try_open(mux_fmt, path, e)) {
    container_ = mux_fmt;
    return true;
  }
  // MP4 失败回退到 TS（更换扩展名）
  if (std::string(mux_fmt) == "mp4") {
    std::string ts_path = path.substr(0, pos == std::string::npos ? path.size() : pos) + ".ts";
    std::fprintf(stdout, "警告 | 视频编码 | MP4 封装失败(%s)，回退到 TS：%s\n",
                 e.c_str(), ts_path.c_str());
    std::fflush(stdout);
    if (try_open("mpegts", ts_path, e)) {
      container_ = "mpegts";
      return true;
    }
  }
  err = e;
  return false;
}

void SophonVideoSink::drain_packets(std::string& err) {
  while (avcodec_receive_packet(enc_, pkt_) >= 0) {
    av_packet_rescale_ts(pkt_, enc_->time_base, vstream_->time_base);
    pkt_->stream_index = vstream_->index;
    if (av_interleaved_write_frame(mux_, pkt_) < 0) {
      err = "封装写帧失败";
    }
    av_packet_unref(pkt_);
  }
}

bool SophonVideoSink::write(VideoFrame& vf, std::string& err) {
  if (!enc_ || !mux_) {
    err = "编码器/封装器未打开";
    return false;
  }
  if (!vf.frame) {
    err = "空帧";
    return false;
  }
  // 按输出帧序重置 pts（编码器 time_base = 1/fps）。
  vf.frame->pts = out_index_++;
  int sr = avcodec_send_frame(enc_, vf.frame);
  if (sr < 0) {
    char eb[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(sr, eb, sizeof(eb));
    err = std::string("编码送帧失败: ") + eb;
    return false;
  }
  drain_packets(err);
  return true;
}

void SophonVideoSink::close() {
  if (enc_ && header_written_) {
    std::string e;
    avcodec_send_frame(enc_, nullptr);  // flush
    drain_packets(e);
  }
  if (mux_ && header_written_) {
    av_write_trailer(mux_);
  }
  if (mux_) {
    if (mux_->pb && !(mux_->oformat->flags & AVFMT_NOFILE)) {
      avio_closep(&mux_->pb);
    }
    avformat_free_context(mux_);
    mux_ = nullptr;
  }
  if (enc_) {
    avcodec_free_context(&enc_);
  }
  if (pkt_) {
    av_packet_free(&pkt_);
  }
  header_written_ = false;
}

}  // namespace hzw
