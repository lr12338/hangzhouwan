// -*- coding: utf-8 -*-
// Bridge Capture 纯逻辑测试（状态机/ROI/padding/评分/文件命名）。无硬件依赖。
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

#include "capture/capture_core.h"
#include "capture/capture_engine.h"

static int failures = 0;
#define CHECK(cond) do { if(!(cond)){ std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while(0)

void test_state_names_and_terminal() {
  using S = hzw::CaptureState;
  CHECK(std::string(hzw::capture_state_name(S::IDLE)) == "IDLE");
  CHECK(std::string(hzw::capture_state_name(S::OPENING_STREAM)) == "OPENING_STREAM");
  CHECK(std::string(hzw::capture_state_name(S::CAPTURED)) == "CAPTURED");
  CHECK(hzw::capture_state_terminal(S::CAPTURED));
  CHECK(hzw::capture_state_terminal(S::FAILED));
  CHECK(hzw::capture_state_terminal(S::TIMEOUT));
  CHECK(!hzw::capture_state_terminal(S::IDLE));
  CHECK(!hzw::capture_state_terminal(S::SEARCHING));
}

void test_roi() {
  hzw::CaptureRoi roi;
  roi.valid_x1 = 0.2; roi.valid_y1 = 0.2; roi.valid_x2 = 0.8; roi.valid_y2 = 0.8;
  roi.cap_x1 = 0.4; roi.cap_y1 = 0.4; roi.cap_x2 = 0.6; roi.cap_y2 = 0.6;
  roi.forbidden = {{0.0, 0.0, 0.1, 0.1}};
  CHECK(roi.point_in_valid(0.5, 0.5));
  CHECK(!roi.point_in_valid(0.1, 0.1));
  CHECK(roi.point_in_capture(0.5, 0.5));
  CHECK(!roi.point_in_capture(0.3, 0.3));
  CHECK(roi.point_in_forbidden(0.05, 0.05));
  CHECK(!roi.point_in_forbidden(0.5, 0.5));
}

void test_pad_and_clamp() {
  // bbox (100,100)-(200,200)，padding 0.15 -> 各扩 15px，未越界
  auto pb = hzw::pad_and_clamp_box(100, 100, 200, 200, 0.15f, 1920, 1080);
  CHECK(std::abs(pb.x1 - 85.0f) < 0.5f);
  CHECK(std::abs(pb.y1 - 85.0f) < 0.5f);
  CHECK(std::abs(pb.x2 - 215.0f) < 0.5f);
  CHECK(std::abs(pb.y2 - 215.0f) < 0.5f);
  // 贴边 clamp
  auto pb2 = hzw::pad_and_clamp_box(0, 0, 50, 50, 0.5f, 100, 100);
  CHECK(pb2.x1 == 0.0f);
  CHECK(pb2.y1 == 0.0f);
  CHECK(pb2.x2 <= 99.0f);
  CHECK(pb2.y2 <= 99.0f);
}

void test_score_candidate() {
  hzw::CaptureRoi roi;
  roi.cap_x1 = 0.4; roi.cap_y1 = 0.4; roi.cap_x2 = 0.6; roi.cap_y2 = 0.6;
  // 居中、大目标、高置信
  auto s1 = hzw::score_candidate(800, 400, 1120, 680, 0.9f, 1920, 1080, roi, 0, 0, false);
  // 偏离中心、小目标、低置信
  auto s2 = hzw::score_candidate(100, 100, 120, 120, 0.3f, 1920, 1080, roi, 0, 0, false);
  CHECK(s1.score > s2.score);
  CHECK(s1.score > 0.0f);
  CHECK(s2.score >= 0.0f);
  // 连续性：相同位置高连续性
  auto s3 = hzw::score_candidate(800, 400, 1120, 680, 0.9f, 1920, 1080, roi, 960, 540, true);
  CHECK(s3.continuity > 0.5f);
  // 贴边惩罚
  auto s4 = hzw::score_candidate(0, 0, 40, 40, 0.9f, 1920, 1080, roi, 0, 0, false);
  CHECK(s4.edge_penalty > 0.0f);
}

void test_filename() {
  // 2026-08-12 04:15:53 UTC = 1786506953000 ms
  std::string fn = hzw::build_capture_filename("north", "414402810", 1786506953000LL, "abc12345-6789");
  CHECK(fn == "north_414402810_20260812035553_abc12345.jpg");
  // session_id 短于 8 字符
  std::string fn2 = hzw::build_capture_filename("south", "1", 0, "x");
  CHECK(fn2.find("south_1_") == 0);
  CHECK(fn2.find("_x.jpg") != std::string::npos);
}

int main() {
  test_state_names_and_terminal();
  test_roi();
  test_pad_and_clamp();
  test_score_candidate();
  test_filename();
  {
    hzw::CaptureEngineConfig cfg;
    CHECK(cfg.disk_free_threshold_mb == 500);
    CHECK(cfg.disk_free_threshold_mb > 0);
  }
  if (failures == 0) { std::printf("capture_core: all passed\n"); return 0; }
  std::printf("capture_core: %d failures\n", failures);
  return 1;
}
