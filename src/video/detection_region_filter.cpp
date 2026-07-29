// -*- coding: utf-8 -*-
#include "video/detection_region_filter.h"

#include <algorithm>

namespace hzw {

void DetectionRegionFilter::configure(const std::vector<Rect>& rectangles,
                                      const std::vector<std::vector<Point>>& polygons,
                                      int ref_width, int ref_height,
                                      int actual_width, int actual_height) {
  enabled_ = true;
  rects_.clear();
  polys_.clear();

  // 按比例换算：参考分辨率 -> 实际分辨率
  double sx = 1.0, sy = 1.0;
  if (ref_width > 0 && actual_width > 0) sx = static_cast<double>(actual_width) / ref_width;
  if (ref_height > 0 && actual_height > 0) sy = static_cast<double>(actual_height) / ref_height;

  for (const auto& r : rectangles) {
    rects_.push_back({r.x1 * sx, r.y1 * sy, r.x2 * sx, r.y2 * sy});
  }
  for (const auto& poly : polygons) {
    std::vector<Point> scaled;
    scaled.reserve(poly.size());
    for (const auto& p : poly) {
      scaled.push_back({p.x * sx, p.y * sy});
    }
    polys_.push_back(std::move(scaled));
  }
}

bool DetectionRegionFilter::point_in_rect(double x, double y, const Rect& r) {
  // 旧语义：含边界（rect_x1 <= x <= rect_x2 且 rect_y1 <= y <= rect_y2）
  return x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2;
}

bool DetectionRegionFilter::point_in_polygon(double x, double y,
                                              const std::vector<Point>& poly) {
  // 射线法（ray casting），与旧 Python is_point_in_polygon 一致：
  // 半开边界 y > min(p1y, p2y) 且 y <= max(p1y, p2y)。
  int n = static_cast<int>(poly.size());
  if (n < 3) return false;
  bool inside = false;
  double p1x = poly[0].x, p1y = poly[0].y;
  for (int i = 1; i <= n; ++i) {
    double p2x = poly[i % n].x;
    double p2y = poly[i % n].y;
    if (y > std::min(p1y, p2y)) {
      if (y <= std::max(p1y, p2y)) {
        if (x <= std::max(p1x, p2x)) {
          if (p1y != p2y) {
            double xinters = (y - p1y) * (p2x - p1x) / (p2y - p1y) + p1x;
            if (p1x == p2x || x <= xinters) {
              inside = !inside;
            }
          }
        }
      }
    }
    p1x = p2x;
    p1y = p2y;
  }
  return inside;
}

bool DetectionRegionFilter::is_in_forbidden(double cx, double cy) const {
  for (const auto& r : rects_) {
    if (point_in_rect(cx, cy, r)) return true;
  }
  for (const auto& p : polys_) {
    if (point_in_polygon(cx, cy, p)) return true;
  }
  return false;
}

void DetectionRegionFilter::filter(std::vector<Detection>& dets) const {
  if (!enabled_) return;
  dets.erase(std::remove_if(dets.begin(), dets.end(),
      [this](const Detection& d) {
        double cx = (static_cast<double>(d.x1) + d.x2) / 2.0;
        double cy = (static_cast<double>(d.y1) + d.y2) / 2.0;
        return is_in_forbidden(cx, cy);
      }), dets.end());
}

}  // namespace hzw
