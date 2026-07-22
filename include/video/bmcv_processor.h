// -*- coding: utf-8 -*-
// =============================================================================
// BmcvProcessor：BM1684 BMCV 硬件预处理与绘制（阶段4 性能优化）。
//
// 基于 libsophon 0.4.9 板端真实头文件实测：
//   - bmcv_image_storage_convert：NV12 -> RGB_PACKED CSC，约 0.3ms（板端 sws 无 SIMD，
//     NV12->RGB 约 97ms，BMCV CSC 快约 300 倍）。
//   - bmcv_image_draw_rectangle：直接在 NV12 bm_image 上绘制矩形，约 0.4ms（CPU 路径
//     需 NV12->RGB->绘框->RGB->NV12 往返约 132ms）。
//   - BMCV VPP resize / yuv_resize / bmcv_image_resize 在本板均不可用（返回不支持或
//     缩放比超限），故 resize 仍由 libswscale 完成（RGB_PACKED 960x544 -> 640x640 约 38ms）。
//   - bmcv_image_convert_to 不支持 RGB_PACKED 且慢于优化 CPU 归一化，故归一化在 CPU 完成。
//
// 资源复用：所有 bm_image 和 sws 上下文在 init 时创建，每帧复用，析构时统一释放。
// 使用 PIMPL 隐藏 BMCV 类型，避免 BMCV 头文件污染上层。
//
// 模型契约（与 CPU 路径完全一致）：RGB / 直接 resize 640x640 / 不 letterbox / 除 255 / NCHW FLOAT32。
// =============================================================================
#ifndef HZW_VIDEO_BMCV_PROCESSOR_H
#define HZW_VIDEO_BMCV_PROCESSOR_H

#include <cstdint>
#include <string>
#include <vector>
#include "inference/yolov7_postprocess.h"  // Detection
#include "video/ffmpeg_compat.h"            // AVFrame

namespace hzw {

class BmcvProcessor {
 public:
  BmcvProcessor();
  ~BmcvProcessor();

  BmcvProcessor(const BmcvProcessor&) = delete;
  BmcvProcessor& operator=(const BmcvProcessor&) = delete;

  // handle_void 为 BmrtDetector::handle() 返回的 bm_handle_t（以 void* 暴露）。
  // src_w/src_h 为解码输出尺寸（如 960x544）。成功返回 true。
  bool init(void* handle_void, int src_w, int src_h, std::string& err);

  // BMCV 预处理：AVFrame(NV12) -> FLOAT32 NCHW [1,3,640,640]。
  // 链路：H2D NV12 -> BMCV CSC NV12->RGB_PACKED(src_w x src_h) -> D2H ->
  //       sws resize RGB_PACKED -> 640x640 -> CPU 归一化 /255 NCHW。
  // out 需预分配 3*640*640 float。返回 false 时 err 给出原因。
  bool preprocess(AVFrame* frame, float* out, std::string& err);

  // BMCV 绘制：在 AVFrame(NV12) 上 in-place 绘制检测矩形。
  // 链路：H2D NV12 -> BMCV draw_rectangle -> D2H 写回 AVFrame。
  // 线宽固定 3，颜色绿色 (0,255,0)，与 CPU 路径一致。
  bool draw_rectangles(AVFrame* frame, const std::vector<Detection>& dets,
                       std::string& err);

  // 带颜色的矩形（融合绘制：绿=AIS匹配，黄=坐标有效未匹配，红=业务不可用）。
  struct ColoredRect {
    float x1, y1, x2, y2;
    unsigned char r, g, b;
  };
  // 在 AVFrame(NV12) 上 in-place 绘制带颜色的矩形。
  bool draw_colored_rectangles(AVFrame* frame,
                               const std::vector<ColoredRect>& rects,
                               std::string& err);

  bool ready() const;

 private:
  struct Impl;
  Impl* p_;
};

}  // namespace hzw

#endif  // HZW_VIDEO_BMCV_PROCESSOR_H
