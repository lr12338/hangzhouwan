// -*- coding: utf-8 -*-
#include "inference/yolov7_postprocess.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hzw {

float iou_xyxy(float ax1, float ay1, float ax2, float ay2,
               float bx1, float by1, float bx2, float by2) {
  float inter_x1 = std::max(ax1, bx1);
  float inter_y1 = std::max(ay1, by1);
  float inter_x2 = std::min(ax2, bx2);
  float inter_y2 = std::min(ay2, by2);
  float iw = inter_x2 - inter_x1;
  float ih = inter_y2 - inter_y1;
  if (iw <= 0.f || ih <= 0.f) return 0.f;
  float inter = iw * ih;
  float a = std::max(0.f, ax2 - ax1) * std::max(0.f, ay2 - ay1);
  float b = std::max(0.f, bx2 - bx1) * std::max(0.f, by2 - by1);
  float uni = a + b - inter;
  if (uni <= 0.f) return 0.f;
  return inter / uni;
}

namespace {
struct Cand {
  float score;
  float x1, y1, x2, y2;
};
}  // namespace

void postprocess_yolov7(const float* output, int num_boxes, int num_vals,
                        int orig_w, int orig_h, int input_size,
                        float conf_threshold, float iou_threshold,
                        std::vector<Detection>& detections,
                        int max_candidates) {
  detections.clear();
  if (output == nullptr || num_boxes <= 0 || num_vals < 6 ||
      orig_w <= 0 || orig_h <= 0 || input_size <= 0) {
    return;
  }
  if (conf_threshold < 0.f) conf_threshold = 0.f;
  if (iou_threshold < 0.f) iou_threshold = 0.f;

  const float sx = static_cast<float>(orig_w) / static_cast<float>(input_size);
  const float sy = static_cast<float>(orig_h) / static_cast<float>(input_size);

  std::vector<Cand> cands;
  cands.reserve(256);
  for (int i = 0; i < num_boxes; ++i) {
    const float* row = output + static_cast<size_t>(i) * num_vals;
    float cx = row[0];
    float cy = row[1];
    float w = row[2];
    float h = row[3];
    float obj_conf = row[4];
    float cls_conf = row[5];

    // 防止 NaN / Inf
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(w) ||
        !std::isfinite(h) || !std::isfinite(obj_conf) || !std::isfinite(cls_conf)) {
      continue;
    }
    // 1) object_confidence 过滤
    if (!(obj_conf > conf_threshold)) continue;
    // 2) score = object_confidence * class_confidence
    float score = obj_conf * cls_conf;
    // 3) score 过滤
    if (!(score > conf_threshold)) continue;

    // 4) 坐标转换：640 空间 cx,cy,w,h -> xyxy，再映射回原图
    float x1 = (cx - w * 0.5f) * sx;
    float y1 = (cy - h * 0.5f) * sy;
    float x2 = (cx + w * 0.5f) * sx;
    float y2 = (cy + h * 0.5f) * sy;

    // 裁剪到图像边界
    x1 = std::max(0.f, std::min(x1, static_cast<float>(orig_w)));
    y1 = std::max(0.f, std::min(y1, static_cast<float>(orig_h)));
    x2 = std::max(0.f, std::min(x2, static_cast<float>(orig_w)));
    y2 = std::max(0.f, std::min(y2, static_cast<float>(orig_h)));

    // 删除宽高 <= 0 的框
    if (x2 <= x1 || y2 <= y1) continue;

    cands.push_back(Cand{score, x1, y1, x2, y2});
  }

  // 按 score 降序
  std::sort(cands.begin(), cands.end(),
            [](const Cand& a, const Cand& b) { return a.score > b.score; });

  // CPU 保护：限制进入 NMS 的候选数量
  if (max_candidates > 0 && static_cast<int>(cands.size()) > max_candidates) {
    cands.resize(max_candidates);
  }

  // 5) 单类别贪心 NMS（xyxy，IoU 阈值）
  std::vector<char> suppressed(cands.size(), 0);
  for (size_t i = 0; i < cands.size(); ++i) {
    if (suppressed[i]) continue;
    const Cand& keep = cands[i];
    Detection d;
    d.class_id = 0;
    d.class_name = "ship";
    d.score = keep.score;
    d.x1 = keep.x1;
    d.y1 = keep.y1;
    d.x2 = keep.x2;
    d.y2 = keep.y2;
    detections.push_back(d);
    for (size_t j = i + 1; j < cands.size(); ++j) {
      if (suppressed[j]) continue;
      float v = iou_xyxy(keep.x1, keep.y1, keep.x2, keep.y2,
                         cands[j].x1, cands[j].y1, cands[j].x2, cands[j].y2);
      if (v > iou_threshold) suppressed[j] = 1;
    }
  }
}

}  // namespace hzw
