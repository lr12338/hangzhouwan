// -*- coding: utf-8 -*-
// =============================================================================
// SophonVideoSink：Sophon H.264 硬件编码 + 封装输出。
//
// 使用 h264_bm 编码器。编码器要求输入 AVFrame 承载 bm_image（解码帧可直接复用），
// 其 NV12 像素经处理线程 in-place 修改后送入编码。封装优先 MP4，若编码器未提供
// extradata 或 MP4 封装失败，自动回退到 TS/裸 H.264。禁止使用 libx264。
// =============================================================================
#ifndef HZW_VIDEO_VIDEO_SINK_H
#define HZW_VIDEO_VIDEO_SINK_H

#include <cstdint>
#include <string>
#include "video/ffmpeg_compat.h"
#include "video/video_frame.h"

namespace hzw {

class SophonVideoSink {
 public:
  SophonVideoSink() = default;
  ~SophonVideoSink();

  SophonVideoSink(const SophonVideoSink&) = delete;
  SophonVideoSink& operator=(const SophonVideoSink&) = delete;

  // 打开输出并初始化 h264_bm 编码器与封装器。
  bool open(const std::string& path, int width, int height, int fps,
            int bitrate_kbps, int gop, int device,
            const std::string& encoder_name, std::string& err);

  // 写入一帧（按输出帧序重置 pts）。返回 false 表示编码/封装错误。
  bool write(VideoFrame& vf, std::string& err);

  // 刷新编码器并写 trailer。
  void close();

  int64_t output_frames() const { return out_index_; }
  std::string actual_container() const { return container_; }
  std::string actual_encoder() const { return encoder_name_; }

 private:
  bool open_encoder(int width, int height, int fps, int bitrate_kbps, int gop,
                    int device, const std::string& encoder_name, std::string& err);
  bool open_muxer(const std::string& path, std::string& err);
  void drain_packets(std::string& err);

  AVFormatContext* mux_ = nullptr;      // 输出封装上下文
  AVCodecContext* enc_ = nullptr;       // h264_bm 编码器
  AVStream* vstream_ = nullptr;
  AVPacket* pkt_ = nullptr;
  int64_t out_index_ = 0;               // 输出帧序（用于 pts）
  bool header_written_ = false;
  std::string container_;               // 实际容器：mp4/ts/h264
  std::string encoder_name_;
};

}  // namespace hzw

#endif  // HZW_VIDEO_VIDEO_SINK_H
