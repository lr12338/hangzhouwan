// -*- coding: utf-8 -*-
#include "video/video_sink.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <thread>
#include <ctime>

namespace hzw {

// ---- RTMP 重连动作决策（纯逻辑）----
RtmpReconnectAction decide_rtmp_reconnect(int send_frame_err, bool encoder_open) {
  if (!encoder_open) return RtmpReconnectAction::ESCALATE_FATAL;
  if (send_frame_err == AVERROR(ENOMEM)) return RtmpReconnectAction::ESCALATE_FATAL;
  return RtmpReconnectAction::REBUILD_MUXER_ONLY;
}

// ---- RtmpBackoffPolicy ----
// 固定序列：1s, 2s, 5s, 10s，之后封顶 30s。
int64_t RtmpBackoffPolicy::on_failure() {
  static const int64_t seq[] = {1000, 2000, 5000, 10000};
  ++attempts_;
  int64_t backoff;
  if (attempts_ <= 4) {
    backoff = seq[attempts_ - 1];
  } else {
    backoff = 30000;  // 封顶
  }
  last_backoff_ms_ = backoff;
  return backoff;
}

void RtmpBackoffPolicy::on_success() {
  attempts_ = 0;
  last_backoff_ms_ = 0;
}

// ---- SophonVideoSink ----
SophonVideoSink::~SophonVideoSink() { close(); }

void SophonVideoSink::mark_resource_fatal(const std::string& reason) {
  resource_fatal_.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(fatal_mutex_);
    fatal_reason_ = reason;
  }
  std::fprintf(stderr, "致命 | 视频编码 | DEVICE_RESOURCE_FATAL: %s\n", reason.c_str());
  std::fflush(stderr);
}

std::string SophonVideoSink::fatal_reason() const {
  std::lock_guard<std::mutex> lk(fatal_mutex_);
  return fatal_reason_;
}

void SophonVideoSink::sleep_interruptible(int64_t ms) {
  const int64_t step = 50;
  for (int64_t slept = 0; slept < ms; slept += step) {
    if (stop_requested_.load()) return;
    int64_t chunk = (ms - slept < step) ? (ms - slept) : step;
    std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
  }
}

bool SophonVideoSink::open(const std::string& path, int width, int height, int fps,
                           int bitrate_kbps, int gop, int device,
                           const std::string& encoder_name,
                           const std::string& sink_type, std::string& err) {
  sink_type_ = sink_type;
  // 保存重连所需参数
  output_url_ = path;
  saved_width_ = width;
  saved_height_ = height;
  saved_fps_ = fps;
  saved_bitrate_kbps_ = bitrate_kbps;
  saved_gop_ = gop;
  saved_device_ = device;
  saved_encoder_name_ = encoder_name;

  if (!open_encoder(width, height, fps, bitrate_kbps, gop, device, encoder_name, err)) {
    return false;
  }
  pkt_ = av_packet_alloc();

  if (sink_type_ == "rtmp") {
    if (!open_rtmp_muxer(path, err)) {
      teardown_muxer_encoder();  // 清理已打开的编码器，避免半初始化
      return false;
    }
  } else {
    if (!open_file_muxer(path, err)) {
      teardown_muxer_encoder();
      return false;
    }
  }
  std::fprintf(stdout, "信息 | 视频编码 | 编码器：%s 容器：%s %dx%d %dfps %dkbps gop=%d sink=%s\n",
               encoder_name_.c_str(), container_.c_str(), width, height, fps,
               bitrate_kbps, gop, sink_type_.c_str());
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
    avcodec_free_context(&enc_);  // 立即清理半初始化上下文
    if (r == AVERROR(ENOMEM)) {
      mark_resource_fatal(std::string("编码器打开内存不足: ") + eb);
      err = std::string("DEVICE_RESOURCE_FATAL: 编码器打开内存不足: ") + eb;
    } else {
      err = std::string("打开编码器失败: ") + eb;
    }
    return false;
  }
  encoder_name_ = enc->name;  // 实际编码器名
  return true;
}

bool SophonVideoSink::open_file_muxer(const std::string& path, std::string& err) {
  // 依据扩展名选择容器；MP4 失败则回退到 TS。
  std::string ext;
  auto pos = path.find_last_of('.');
  if (pos != std::string::npos) ext = path.substr(pos + 1);
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

bool SophonVideoSink::open_rtmp_muxer(const std::string& url, std::string& err) {
  // RTMP 推流：FLV muxer + RTMP 协议（avio_open2）。复用已打开的 enc_（不重建编码器）。
  if (!enc_) {
    err = "编码器未打开，无法建立 RTMP muxer";
    return false;
  }
  AVFormatContext* m = nullptr;
  if (avformat_alloc_output_context2(&m, nullptr, "flv", url.c_str()) < 0 || !m) {
    err = "分配 FLV 封装失败";
    return false;
  }
  mux_ = m;
  vstream_ = avformat_new_stream(mux_, nullptr);
  if (!vstream_) {
    err = "新建输出流失败";
    avformat_free_context(mux_);
    mux_ = nullptr;
    return false;
  }
  vstream_->time_base = enc_->time_base;
  if (avcodec_parameters_from_context(vstream_->codecpar, enc_) < 0) {
    err = "复制编码参数失败";
    avformat_free_context(mux_);
    mux_ = nullptr;
    return false;
  }
  // 网络输出使用 avio_open2
  AVDictionary* opts = nullptr;
  // 设置写超时（微秒），避免网络挂起时永久阻塞
  av_dict_set_int(&opts, "rw_timeout", 10000000, 0);  // 10 秒写超时
  int r = avio_open2(&mux_->pb, url.c_str(), AVIO_FLAG_WRITE, nullptr, &opts);
  av_dict_free(&opts);
  if (r < 0) {
    char eb[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(r, eb, sizeof(eb));
    err = std::string("RTMP 连接失败: ") + eb;
    avformat_free_context(mux_);
    mux_ = nullptr;
    return false;
  }
  AVDictionary* hdr_opts = nullptr;
  r = avformat_write_header(mux_, &hdr_opts);
  av_dict_free(&hdr_opts);
  if (r < 0) {
    char eb[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(r, eb, sizeof(eb));
    err = std::string("写 FLV 头失败: ") + eb;
    avio_closep(&mux_->pb);
    avformat_free_context(mux_);
    mux_ = nullptr;
    return false;
  }
  header_written_ = true;
  container_ = "flv";
  // 部分 CDN RTMP 服务器在 publish 命令后需要时间处理，首帧写入过早会导致 Broken pipe。
  // 延迟 500ms 等待服务器 onStatus(Publish.Start) 响应。
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  return true;
}

void SophonVideoSink::teardown_muxer() {
  // 仅释放 muxer 与 AVIO，保留编码器（用于 RTMP 网络重连，避免 VPU 显存 churn）。
  if (mux_) {
    if (mux_->pb && !(mux_->oformat->flags & AVFMT_NOFILE)) {
      avio_closep(&mux_->pb);
    }
    avformat_free_context(mux_);
    mux_ = nullptr;
  }
  vstream_ = nullptr;
  header_written_ = false;
}

void SophonVideoSink::teardown_muxer_encoder() {
  teardown_muxer();
  if (enc_) {
    avcodec_free_context(&enc_);
  }
}

bool SophonVideoSink::reconnect_rtmp(std::string& err) {
  // 普通网络断开仅重建 muxer/AVIO，绝不重建硬件编码器。
  // 编码器致命（ENOMEM/未打开）升级 DEVICE_RESOURCE_FATAL，禁止错误风暴。
  if (!enc_) {
    mark_resource_fatal("RTMP重连时编码器不可用");
    err = "DEVICE_RESOURCE_FATAL: 编码器不可用";
    return false;
  }
  teardown_muxer();  // 仅释放 muxer+AVIO，保留 enc_
  while (true) {
    if (stop_requested_.load()) {
      err = "停止";
      return false;
    }
    if (resource_fatal_.load()) {
      err = "DEVICE_RESOURCE_FATAL: " + fatal_reason();
      return false;
    }
    int64_t backoff = rtmp_backoff_.on_failure();
    std::fprintf(stdout,
                 "信息 | RTMP重连 | BACKOFF 第%d次 退避%lldms（仅重建muxer）\n",
                 rtmp_backoff_.attempts(), static_cast<long long>(backoff));
    std::fflush(stdout);
    sleep_interruptible(backoff);
    if (stop_requested_.load()) {
      err = "停止";
      return false;
    }
    // 重建 FLV muxer 并重开 RTMP（复用现有 enc_）
    if (!open_rtmp_muxer(output_url_, err)) {
      std::fprintf(stdout, "信息 | RTMP重连 | RTMP重连失败：%s\n", err.c_str());
      std::fflush(stdout);
      teardown_muxer();  // 清理半开 muxer，继续退避
      continue;
    }
    // 重连成功：重置退避。muxer-only 重连不重建编码器，编码器 DTS 续接，
    // 故 PTS 必须续接（不可 reset），否则 PTS 归零 < DTS 致 FLV 写帧永久失败。
    rtmp_backoff_.on_success();
    ++rtmp_reconnect_count_;
    std::fprintf(stdout, "信息 | RTMP重连 | 重连成功（第%d次，PTS续接，编码器未重建）\n",
                 rtmp_reconnect_count_);
    std::fflush(stdout);
    return true;
  }
}

bool SophonVideoSink::simulate_rtmp_reconnect(const std::string& url, std::string& err) {
  // 测试钩子：强制一次仅重建 muxer 的重连（复用现有编码器）。
  if (!enc_) {
    mark_resource_fatal("simulate_rtmp_reconnect 编码器不可用");
    err = "DEVICE_RESOURCE_FATAL: 编码器不可用";
    return false;
  }
  teardown_muxer();
  return open_rtmp_muxer(url, err);
}

void SophonVideoSink::drain_packets(std::string& err) {
  while (avcodec_receive_packet(enc_, pkt_) >= 0) {
    av_packet_rescale_ts(pkt_, enc_->time_base, vstream_->time_base);
    pkt_->stream_index = vstream_->index;
    if (av_interleaved_write_frame(mux_, pkt_) < 0) {
      err = "封装写帧失败";
    } else {
      total_output_frames_.fetch_add(1);
    }
    av_packet_unref(pkt_);
  }
}

bool SophonVideoSink::write(VideoFrame& vf, std::string& err) {
  if (!enc_ || !mux_) {
    // RTMP 模式下可能因重连暂时无 muxer，尝试重连
    if (sink_type_ == "rtmp") {
      if (!reconnect_rtmp(err)) return false;
    } else {
      err = "编码器/封装器未打开";
      return false;
    }
  }
  if (!vf.frame) {
    err = "空帧";
    return false;
  }
  // 按输出帧序重置 pts（编码器 time_base = 1/fps）；忽略源 PTS，保证输出单调。
  vf.frame->pts = pts_.next();
  int sr = avcodec_send_frame(enc_, vf.frame);
  if (sr == AVERROR(EAGAIN)) {
    drain_packets(err);  // 编码器内部满，先冲刷已编码包再重试
    sr = avcodec_send_frame(enc_, vf.frame);
  }
  if (sr < 0) {
    char eb[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(sr, eb, sizeof(eb));
    if (decide_rtmp_reconnect(sr, enc_ != nullptr) == RtmpReconnectAction::ESCALATE_FATAL) {
      mark_resource_fatal(std::string("编码器内存不足: ") + eb);
      err = std::string("DEVICE_RESOURCE_FATAL: ") + eb;
      return false;
    }
    err = std::string("编码送帧失败: ") + eb;
    return false;
  }
  std::string write_err;
  drain_packets(write_err);
  // RTMP 模式：写入失败时尝试重连并重试
  if (sink_type_ == "rtmp" && !write_err.empty()) {
    std::fprintf(stdout, "信息 | RTMP重连 | 写入失败：%s，进入重连流程\n", write_err.c_str());
    std::fflush(stdout);
    // 重连后 PTS 续接（muxer-only 不重置），重新写入当前帧
    if (!reconnect_rtmp(err)) return false;
    // 重连成功后重新写入当前帧
    vf.frame->pts = pts_.next();
    sr = avcodec_send_frame(enc_, vf.frame);
    if (sr == AVERROR(EAGAIN)) {
      drain_packets(err);
      sr = avcodec_send_frame(enc_, vf.frame);
    }
    if (sr < 0) {
      char eb[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(sr, eb, sizeof(eb));
      if (decide_rtmp_reconnect(sr, enc_ != nullptr) == RtmpReconnectAction::ESCALATE_FATAL) {
        mark_resource_fatal(std::string("重连后编码器内存不足: ") + eb);
        err = std::string("DEVICE_RESOURCE_FATAL: ") + eb;
        return false;
      }
      err = std::string("重连后编码送帧失败: ") + eb;
      return false;
    }
    drain_packets(err);
  } else if (!write_err.empty()) {
    err = write_err;
    return false;
  }
  return true;
}

void SophonVideoSink::close() {
  if (enc_ && header_written_ && mux_) {
    std::string e;
    avcodec_send_frame(enc_, nullptr);  // flush
    drain_packets(e);
  }
  if (mux_ && header_written_) {
    av_write_trailer(mux_);
  }
  teardown_muxer_encoder();
  if (pkt_) {
    av_packet_free(&pkt_);
  }
}

}  // namespace hzw
