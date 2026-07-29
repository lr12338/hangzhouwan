// -*- coding: utf-8 -*-
// YOLOv7 后处理单元测试（无外部依赖，断言式）。
// 编译运行：见 tools/image_inference/CMakeLists.txt 的 test_postprocess 目标。
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "inference/yolov7_postprocess.h"

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cerr << "失败 | " << #cond << " @行" << __LINE__ << "\n";    \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

static bool approx(float a, float b, float eps = 1e-3f) {
  return std::fabs(a - b) <= eps;
}

// 构造 num_boxes 个候选的输出缓冲（其余补 0）。
static std::vector<float> make_output(const std::vector<float>& rows,
                                      int num_vals = 6) {
  int num_boxes = static_cast<int>(rows.size()) / num_vals;
  std::vector<float> out(static_cast<size_t>(num_boxes) * num_vals, 0.0f);
  std::memcpy(out.data(), rows.data(), rows.size() * sizeof(float));
  return out;
}

static void test_single_box() {
  // cx,cy,w,h,obj,cls = 320,320,100,100,0.9,0.95 ; 原图 960x544
  std::vector<float> rows = {320, 320, 100, 100, 0.9f, 0.95f};
  auto out = make_output(rows);
  std::vector<hzw::Detection> dets;
  hzw::postprocess_yolov7(out.data(), 1, 6, 960, 544, 640, 0.1f, 0.1f, dets);
  CHECK(dets.size() == 1);
  if (dets.size() == 1) {
    CHECK(approx(dets[0].score, 0.855f));
    // sx=1.5 sy=0.85 ; x1=(320-50)*1.5=405 y1=(320-50)*0.85=229.5
    CHECK(approx(dets[0].x1, 405.0f));
    CHECK(approx(dets[0].y1, 229.5f));
    CHECK(approx(dets[0].x2, 555.0f));
    CHECK(approx(dets[0].y2, 314.5f));
    CHECK(dets[0].class_name == "ship");
    CHECK(dets[0].class_id == 0);
  }
}

static void test_obj_conf_filter() {
  std::vector<float> rows = {320, 320, 100, 100, 0.05f, 0.99f};  // obj<0.1
  auto out = make_output(rows);
  std::vector<hzw::Detection> dets;
  hzw::postprocess_yolov7(out.data(), 1, 6, 960, 544, 640, 0.1f, 0.1f, dets);
  CHECK(dets.size() == 0);
}

static void test_score_filter() {
  // obj=0.9 cls=0.05 -> score=0.045 < 0.1
  std::vector<float> rows = {320, 320, 100, 100, 0.9f, 0.05f};
  auto out = make_output(rows);
  std::vector<hzw::Detection> dets;
  hzw::postprocess_yolov7(out.data(), 1, 6, 960, 544, 640, 0.1f, 0.1f, dets);
  CHECK(dets.size() == 0);
}

static void test_nms_suppresses_overlap() {
  // 两个高度重叠框，高分 A 应保留，低分 B 被抑制
  std::vector<float> rows = {
    320, 320, 100, 100, 0.9f, 0.95f,   // A score=0.855
    322, 320, 100, 100, 0.8f, 0.9f,    // B score=0.72，与 A IoU>0.1
  };
  auto out = make_output(rows);
  std::vector<hzw::Detection> dets;
  hzw::postprocess_yolov7(out.data(), 2, 6, 960, 544, 640, 0.1f, 0.1f, dets);
  CHECK(dets.size() == 1);
  if (dets.size() == 1) {
    CHECK(approx(dets[0].score, 0.855f));  // 保留高分 A
  }
}

static void test_nms_keeps_non_overlap() {
  // 两个不重叠框，均保留
  std::vector<float> rows = {
    100, 100, 40, 40, 0.9f, 0.95f,
    540, 540, 40, 40, 0.8f, 0.9f,
  };
  auto out = make_output(rows);
  std::vector<hzw::Detection> dets;
  hzw::postprocess_yolov7(out.data(), 2, 6, 960, 544, 640, 0.1f, 0.1f, dets);
  CHECK(dets.size() == 2);
}

static void test_zero_output() {
  std::vector<float> rows(6, 0.0f);
  auto out = make_output(rows);
  std::vector<hzw::Detection> dets;
  hzw::postprocess_yolov7(out.data(), 1, 6, 960, 544, 640, 0.1f, 0.1f, dets);
  CHECK(dets.size() == 0);
}

static void test_clip_to_bounds() {
  // 框超出左上边界，应裁剪到 [0,..]
  std::vector<float> rows = {10, 10, 100, 100, 0.9f, 0.95f};
  auto out = make_output(rows);
  std::vector<hzw::Detection> dets;
  hzw::postprocess_yolov7(out.data(), 1, 6, 960, 544, 640, 0.1f, 0.1f, dets);
  CHECK(dets.size() == 1);
  if (dets.size() == 1) {
    CHECK(dets[0].x1 >= 0.0f);
    CHECK(dets[0].y1 >= 0.0f);
    CHECK(dets[0].x2 <= 960.0f);
    CHECK(dets[0].y2 <= 544.0f);
    CHECK(approx(dets[0].x1, 0.0f));
    CHECK(approx(dets[0].y1, 0.0f));
  }
}

static void test_invalid_input() {
  std::vector<hzw::Detection> dets;
  hzw::postprocess_yolov7(nullptr, 1, 6, 960, 544, 640, 0.1f, 0.1f, dets);
  CHECK(dets.size() == 0);
  float buf[6] = {0};
  hzw::postprocess_yolov7(buf, 1, 6, 0, 0, 640, 0.1f, 0.1f, dets);  // orig 0
  CHECK(dets.size() == 0);
}

static void test_max_candidates_cap() {
  // 3 个不重叠高分框，max_candidates=2 -> 最多保留 2
  std::vector<float> rows = {
    100, 100, 40, 40, 0.9f, 0.95f,
    300, 300, 40, 40, 0.85f, 0.95f,
    540, 540, 40, 40, 0.8f, 0.9f,
  };
  auto out = make_output(rows);
  std::vector<hzw::Detection> dets;
  hzw::postprocess_yolov7(out.data(), 3, 6, 960, 544, 640, 0.1f, 0.1f, dets, 2);
  CHECK(dets.size() == 2);
}

static void test_iou_helper() {
  // 完全重叠 IoU=1
  CHECK(approx(hzw::iou_xyxy(0, 0, 10, 10, 0, 0, 10, 10), 1.0f));
  // 不重叠 IoU=0
  CHECK(approx(hzw::iou_xyxy(0, 0, 10, 10, 20, 20, 30, 30), 0.0f));
  // 半重叠：两个 10x10，交集 5x10=50，并集 100+100-50=150 -> 0.333
  CHECK(approx(hzw::iou_xyxy(0, 0, 10, 10, 5, 0, 15, 10), 50.0f / 150.0f));
}

int main() {
  test_single_box();
  test_obj_conf_filter();
  test_score_filter();
  test_nms_suppresses_overlap();
  test_nms_keeps_non_overlap();
  test_zero_output();
  test_clip_to_bounds();
  test_invalid_input();
  test_max_candidates_cap();
  test_iou_helper();
  if (g_failures == 0) {
    std::cout << "通过 | YOLOv7 后处理单元测试全部通过\n";
    return 0;
  }
  std::cout << "失败 | 共 " << g_failures << " 项断言未通过\n";
  return 1;
}
