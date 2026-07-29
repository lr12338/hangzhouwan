// -*- coding: utf-8 -*-
// StreamProfile 解析单元测试：A/B 配置解析、必填字段校验。
// 纯逻辑，不依赖硬件。配置中使用占位 URL（不含真实凭据）。
#include <iostream>
#include <string>
#include "config/stream_profile.h"

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cerr << "失败 | " << #cond << " @行" << __LINE__ << "\n";    \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

static const char* TEST_YAML = R"(
environment: internal-development
streams:
  A:
    enabled: true
    input_url: rtsp://STREAM_A_INPUT_URL
    output_url: rtmp://STREAM_A_OUTPUT_URL
    coordinate_model: /path/to/beixia_model.pkl
    camera_param: 0.5
    forbidden_rectangles:
      - [1480, 0, 2560, 630]
      - [247, 855, 275, 888]
    forbidden_polygons:
      - [[0, 0], [0, 640], [710, 620], [1260, 620], [1260, 0]]
  B:
    enabled: false
    input_url: rtsp://STREAM_B_INPUT_URL
    output_url: rtmp://STREAM_B_OUTPUT_URL
    coordinate_model: /path/to/beishang_model.pkl
    camera_param: 0.8
    forbidden_rectangles:
      - [0, 0, 2560, 210]
    forbidden_polygons:
      - [[2160, 210], [2560, 280], [2560, 210]]
)";

int main() {
  using hzw::StreamProfile;
  using hzw::parse_stream_profile;
  using hzw::validate_stream_profile;

  // 1) 解析 A 路配置
  {
    StreamProfile p; std::string err;
    CHECK(parse_stream_profile(TEST_YAML, "A", p, err));
    CHECK(p.stream_id == "A");
    CHECK(p.enabled);
    CHECK(p.input_url == "rtsp://STREAM_A_INPUT_URL");
    CHECK(p.output_url == "rtmp://STREAM_A_OUTPUT_URL");
    CHECK(p.coordinate_model_path == "/path/to/beixia_model.pkl");
    CHECK(p.camera_param == 0.5);
    CHECK(p.forbidden_rectangles.size() == 2);
    CHECK(p.forbidden_rectangles[0].x1 == 1480);
    CHECK(p.forbidden_rectangles[0].x2 == 2560);
    CHECK(p.forbidden_rectangles[1].x1 == 247);
    CHECK(p.forbidden_polygons.size() == 1);
    CHECK(p.forbidden_polygons[0].size() == 5);
    CHECK(p.forbidden_polygons[0][0].x == 0);
    CHECK(p.forbidden_polygons[0][2].x == 710);
    std::string verr;
    CHECK(validate_stream_profile(p, verr));
  }

  // 2) 解析 B 路配置
  {
    StreamProfile p; std::string err;
    CHECK(parse_stream_profile(TEST_YAML, "B", p, err));
    CHECK(p.stream_id == "B");
    CHECK(!p.enabled);
    CHECK(p.input_url == "rtsp://STREAM_B_INPUT_URL");
    CHECK(p.output_url == "rtmp://STREAM_B_OUTPUT_URL");
    CHECK(p.camera_param == 0.8);
    CHECK(p.forbidden_rectangles.size() == 1);
    CHECK(p.forbidden_rectangles[0].x1 == 0);
    CHECK(p.forbidden_rectangles[0].y2 == 210);
    CHECK(p.forbidden_polygons.size() == 1);
    CHECK(p.forbidden_polygons[0].size() == 3);
  }

  // 3) 未找到 stream_id
  {
    StreamProfile p; std::string err;
    CHECK(!parse_stream_profile(TEST_YAML, "C", p, err));
    CHECK(!err.empty());
  }

  // 4) 校验空字段
  {
    StreamProfile p; std::string err;
    p.stream_id = "A";
    CHECK(!validate_stream_profile(p, err));  // input_url 为空
    CHECK(err.find("input_url") != std::string::npos);
  }

  if (g_failures == 0) std::cout << "通过 | StreamProfile 解析单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
