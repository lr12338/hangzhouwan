// -*- coding: utf-8 -*-
// =============================================================================
// YOLOv7 后处理（单类别 ship）。
//
// 模型实际输出：[1, 25200, 6]，每行 6 个值按原 Python 解释为：
//   cx, cy, width, height, object_confidence, class_confidence
//
// 语义保持与 hangzhouwan_beishang/detector.py 的 process_output 一致：
//   1) object_confidence > conf_threshold  过滤
//   2) score = object_confidence * class_confidence
//   3) score > conf_threshold             过滤
//   4) 坐标按“直接 resize（非 letterbox）”映射回原图：scale_x = orig_w/640, scale_y = orig_h/640
//   5) 单类别贪心 NMS（IoU 阈值 iou_threshold），按 score 降序
//
// 输出 Detection 为 xyxy（原图坐标，已裁剪到图像边界）。
// =============================================================================
#ifndef HZW_INFERENCE_YOLOV7_POSTPROCESS_H
#define HZW_INFERENCE_YOLOV7_POSTPROCESS_H

#include <string>
#include <vector>

namespace hzw {

struct Detection {
  int class_id = 0;
  std::string class_name;   // 单类别模型固定 "ship"
  float score = 0.0f;
  float x1 = 0.0f;  // 原图坐标，左上 x
  float y1 = 0.0f;  // 原图坐标，左上 y
  float x2 = 0.0f;  // 原图坐标，右下 x
  float y2 = 0.0f;  // 原图坐标，右下 y
};

// output        : 模型输出缓冲（长度应 >= num_boxes * num_vals）
// num_boxes     : 候选框数量（预期 25200）
// num_vals      : 每框数值个数（预期 6）
// orig_w/orig_h : 原图宽高（用于坐标映射回原图）
// input_size    : 模型输入边长（640）
// max_candidates: 进入 NMS 前按 score 保留的最大候选数（CPU 保护，默认 5000）
//
// 返回 detections（已 NMS、按 score 降序）。
void postprocess_yolov7(const float* output, int num_boxes, int num_vals,
                        int orig_w, int orig_h, int input_size,
                        float conf_threshold, float iou_threshold,
                        std::vector<Detection>& detections,
                        int max_candidates = 5000);

// 计算两个 xyxy 框的 IoU（供测试/复用）。
float iou_xyxy(float ax1, float ay1, float ax2, float ay2,
               float bx1, float by1, float bx2, float by2);

}  // namespace hzw

#endif  // HZW_INFERENCE_YOLOV7_POSTPROCESS_H
