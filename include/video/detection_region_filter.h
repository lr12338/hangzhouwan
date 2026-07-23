// -*- coding: utf-8 -*-
// =============================================================================
// DetectionRegionFilter：禁区过滤（阶段4.3）。
//
// 旧 Python 语义审计（历史 hangzhouwan_beishang/utils_demo/method.py，已移除）：
//   - 使用检测框中心点（(x1+x2)/2, (y1+y2)/2），不是框边缘。
//   - 使用包含关系（中心点是否在禁区内），不是交集。
//   - 过滤发生在 NMS 后、绘框前。
//   - 矩形判断：rect_x1 <= x <= rect_x2 且 rect_y1 <= y <= rect_y2（含边界）。
//   - 多边形判断：射线法（ray casting），半开边界（y > min, y <= max）。
//   - 坐标参考原始摄像头分辨率（如 2560x1440）；实际分辨率不同则按比例换算。
//
// 处理顺序：YOLO后处理 -> NMS -> 禁区过滤 -> 绘框。
// 纯逻辑，不依赖硬件/FFmpeg，可独立单元测试。
// =============================================================================
#ifndef HZW_VIDEO_DETECTION_REGION_FILTER_H
#define HZW_VIDEO_DETECTION_REGION_FILTER_H

#include <cstdint>
#include <string>
#include <vector>
#include "inference/yolov7_postprocess.h"  // Detection

namespace hzw {

struct Point {
  double x = 0;
  double y = 0;
};

struct Rect {
  double x1 = 0;
  double y1 = 0;
  double x2 = 0;
  double y2 = 0;
};

class DetectionRegionFilter {
 public:
  DetectionRegionFilter() = default;

  // 配置禁区与参考分辨率。ref_width/ref_height 为禁区坐标的原始参考分辨率。
  // actual_width/actual_height 为实际视频分辨率；不同时按比例换算禁区坐标。
  void configure(const std::vector<Rect>& rectangles,
                 const std::vector<std::vector<Point>>& polygons,
                 int ref_width, int ref_height,
                 int actual_width, int actual_height);

  bool enabled() const { return enabled_; }

  // 过滤检测框：移除中心点落在禁区内的检测。返回过滤后的检测结果。
  void filter(std::vector<Detection>& dets) const;

  // 纯逻辑判断：点是否在任一禁区内（含已换算的坐标）。
  bool is_in_forbidden(double cx, double cy) const;

 private:
  bool enabled_ = false;
  // 已换算到实际分辨率的禁区坐标
  std::vector<Rect> rects_;
  std::vector<std::vector<Point>> polys_;

  static bool point_in_rect(double x, double y, const Rect& r);
  static bool point_in_polygon(double x, double y, const std::vector<Point>& poly);
};

}  // namespace hzw

#endif  // HZW_VIDEO_DETECTION_REGION_FILTER_H
