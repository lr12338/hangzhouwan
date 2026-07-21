// -*- coding: utf-8 -*-
// =============================================================================
// 视频帧结构：承载 Sophon 硬件解码输出的 AVFrame（bm_image，NV12，host 可读写）。
//
// 所有权：frame 持有 AVFrame 的一个引用计数，持有者必须调用 release()（等价
// av_frame_unref）归还给解码器缓冲池。禁止把已 release 的帧放入队列。
// 生命周期：解码线程产生 -> 队列 -> 处理线程(in-place 修改 NV12) -> 队列 -> 编码线程
//           消费后 release。bm_image 设备内存在 release 后由解码器回收。
// =============================================================================
#ifndef HZW_VIDEO_VIDEO_FRAME_H
#define HZW_VIDEO_VIDEO_FRAME_H

#include <cstdint>
#include "video/ffmpeg_compat.h"

namespace hzw {

struct VideoFrame {
  int64_t sequence = 0;          // 解码序号（从 0 单调递增）
  int64_t pts = 0;               // 源 PTS（按源 time_base）
  int64_t capture_time_ms = 0;   // 解码完成时刻（墙钟 ms）
  int source_epoch = 0;          // 源连接 epoch（每次重连 +1），用于清除过期检测结果
  int width = 0;
  int height = 0;
  AVFrame* frame = nullptr;      // NV12 bm_image；持有引用，需 release()

  bool valid() const { return frame != nullptr; }

  // 移动语义：转移 AVFrame 所有权，源置空，避免双重释放。
  VideoFrame() = default;
  VideoFrame(VideoFrame&& o) noexcept { *this = std::move(o); }
  VideoFrame& operator=(VideoFrame&& o) noexcept {
    release();
    sequence = o.sequence; pts = o.pts; capture_time_ms = o.capture_time_ms;
    source_epoch = o.source_epoch; width = o.width; height = o.height; frame = o.frame;
    o.frame = nullptr;
    return *this;
  }
  VideoFrame(const VideoFrame&) = delete;
  VideoFrame& operator=(const VideoFrame&) = delete;

  // 归还引用计数（线程安全：同一 frame 仅能 release 一次）。
  void release() {
    if (frame) {
      av_frame_unref(frame);
      av_frame_free(&frame);
      frame = nullptr;
    }
  }
};

}  // namespace hzw

#endif  // HZW_VIDEO_VIDEO_FRAME_H
