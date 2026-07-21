// -*- coding: utf-8 -*-
// =============================================================================
// SophonVideoSource：Sophon H.264 硬件解码视频源。
//
// 使用 Sophon-FFmpeg C API（avcodec + h264_bm）打开本地文件、硬件解码为 NV12 帧。
// 解码帧的 AVFrame 承载 bm_image，其 data[] 为 host 可读写的 mmap 设备内存。
// 支持 loop：N>0 播放 N 遍，0 表示无限循环直到外部停止。
// =============================================================================
#ifndef HZW_VIDEO_VIDEO_SOURCE_H
#define HZW_VIDEO_VIDEO_SOURCE_H

#include <cstdint>
#include <string>
#include "video/ffmpeg_compat.h"
#include "video/video_frame.h"

namespace hzw {

class SophonVideoSource {
 public:
  SophonVideoSource() = default;
  ~SophonVideoSource();

  SophonVideoSource(const SophonVideoSource&) = delete;
  SophonVideoSource& operator=(const SophonVideoSource&) = delete;

  // 打开输入文件并初始化 h264_bm 解码器。extra_frame_buffer_num 控制解码输出缓冲池
  // （需 >= 编码器保留帧数，默认 20 以避免 bm_image 池死锁）。
  bool open(const std::string& path, int device, int extra_frame_buffer_num,
            const std::string& decoder_name, std::string& err);

  // 设置循环次数（0=无限）。需在 open 之后、read 之前调用。
  void set_loop(int n) { loop_ = n; }

  // 读取一帧到 vf（所有权转移给调用者，需 vf.release()）。
  // 返回 false 表示 EOF（且 loop 已用尽）或错误；err 给出原因。
  bool read(VideoFrame& vf, std::string& err);

  int width() const { return width_; }
  int height() const { return height_; }
  int fps() const { return fps_; }
  AVRational time_base() const { return time_base_; }
  int64_t sequence() const { return seq_; }
  int64_t packets_read() const { return packets_read_; }
  int loops_completed() const { return loops_completed_; }

  void close();

 private:
  bool open_decoder(const std::string& decoder_name, std::string& err);
  void close_decoder();
  bool seek_to_start(std::string& err);

  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* dec_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
  int video_index_ = -1;
  int width_ = 0, height_ = 0, fps_ = 0;
  AVRational time_base_{1, 1};
  int64_t seq_ = 0;
  int64_t packets_read_ = 0;
  int loop_ = 1;                   // 0 = 无限
  int loops_completed_ = 0;
  int device_ = 0;
  int extra_frame_buffer_num_ = 20;
  std::string path_;
};

}  // namespace hzw

#endif  // HZW_VIDEO_VIDEO_SOURCE_H
