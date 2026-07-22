// -*- coding: utf-8 -*-
// =============================================================================
// 双路并发视频推理 CLI（阶段4.4）。
//
// A/B 同时启动，各自独立 RTSP/RTMP/推理/编码。全局 SIGINT/SIGTERM 同时停止。
// 真实 RTSP/RTMP 地址从环境变量读取，不入命令行/日志。
//
// 用法见 tools/dual_stream/README.md。
// =============================================================================
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "application/dual_stream_application.h"
#include "video/detection_region_filter.h"

namespace {

hzw::DualStreamApplication* g_app = nullptr;

void on_signal(int sig) {
  if (g_app) {
    std::fprintf(stdout, "\n信息 | 信号 | 收到信号 %d，停止双路\n", sig);
    std::fflush(stdout);
    g_app->request_stop();
  }
}

std::string env_or_empty(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

bool env_required(const char* name, std::string& out) {
  out = env_or_empty(name);
  if (out.empty()) {
    std::fprintf(stderr, "错误 | 配置 | 环境变量 %s 未设置\n", name);
    return false;
  }
  return true;
}

// A 路禁区（参考分辨率 2560x1440）
void setup_a_forbidden_zones(hzw::PipelineConfig& cfg) {
  cfg.enable_region_filter = true;
  cfg.region_ref_width = 2560;
  cfg.region_ref_height = 1440;
  cfg.forbidden_rectangles = {
    {1480, 0, 2560, 630},
    {247, 855, 275, 888},
    {2295, 895, 2315, 927},
  };
  cfg.forbidden_polygons = {
    {{0, 0}, {0, 640}, {710, 620}, {1260, 620}, {1260, 0}},
  };
}

// B 路禁区（参考分辨率 2560x1440）
void setup_b_forbidden_zones(hzw::PipelineConfig& cfg) {
  cfg.enable_region_filter = true;
  cfg.region_ref_width = 2560;
  cfg.region_ref_height = 1440;
  cfg.forbidden_rectangles = {
    {0, 0, 2560, 210},
  };
  cfg.forbidden_polygons = {
    {{2160, 210}, {2560, 280}, {2560, 210}},
  };
}

void setup_common(hzw::PipelineConfig& cfg) {
  cfg.bmodel_path = "artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel";
  cfg.device = 0;
  cfg.decoder = "h264_bm";
  cfg.encoder = "h264_bm";
  cfg.source_fps = 25;
  cfg.output_fps = 10;
  cfg.inference_fps = 5;
  cfg.bitrate_kbps = 800;
  cfg.gop = 20;
  cfg.queue_size = 1;
  cfg.conf = 0.1f;
  cfg.iou = 0.1f;
  cfg.result_ttl_ms = 1000;
  cfg.preprocess = "bmcv";
  cfg.draw_mode = "bmcv";
  cfg.source_type = "rtsp";
  cfg.rtsp_transport = "tcp";
  cfg.rtsp_stimeout_us = 5000000;
  cfg.rtsp_max_reconnect = -1;
  cfg.rtsp_initial_backoff_ms = 1000;
  cfg.rtsp_max_backoff_ms = 30000;
  cfg.sink_type = "rtmp";
  cfg.jitter_buffer_size = 5;
  cfg.metrics_interval_sec = 10;
}

void usage() {
  std::fprintf(stdout,
      "用法: dual_stream_app [选项]\n"
      "  --max-seconds 300       最长运行秒数（0=不限）\n"
      "  --metrics-interval 10   指标输出间隔秒\n"
      "  --detector-mode per_stream  per_stream | shared_serialized\n"
      "  --jitter-buffer-size 5  抖动缓冲帧数\n"
      "  --output-fps 10         输出帧率\n"
      "  --inference-fps 5       推理帧率\n"
      "  --no-region-filter      禁用禁区过滤\n"
      "  --streams A,B           指定启动的流（默认 A,B）\n"
  "  --enable-business        启用业务增强（坐标+AIS）\n"
  "  --business-socket PATH  Sidecar Unix Socket 路径\n"
      "\n"
      "环境变量:\n"
      "  STREAM_A_INPUT_URL / STREAM_A_OUTPUT_URL\n"
      "  STREAM_B_INPUT_URL / STREAM_B_OUTPUT_URL\n");
}

}  // namespace

int main(int argc, char** argv) {
  int max_seconds = 0;
  int metrics_interval = 10;
  std::string detector_mode = "per_stream";
  int jitter_buffer_size = 5;
  int output_fps = 10;
  int inference_fps = 5;
  bool no_region_filter = false;
  std::string streams = "A,B";
  bool enable_business = false;
  std::string business_socket = "/tmp/hangzhouwan-business.sock";

  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "缺少 %s 的值\n", name); return nullptr; }
      return argv[++i];
    };
    if (k == "--help" || k == "-h") { usage(); return 0; }
    else if (k == "--max-seconds") { auto v = next(k.c_str()); if (v) max_seconds = std::atoi(v); else return 1; }
    else if (k == "--metrics-interval") { auto v = next(k.c_str()); if (v) metrics_interval = std::atoi(v); else return 1; }
    else if (k == "--detector-mode") { auto v = next(k.c_str()); if (v) detector_mode = v; else return 1; }
    else if (k == "--jitter-buffer-size") { auto v = next(k.c_str()); if (v) jitter_buffer_size = std::atoi(v); else return 1; }
    else if (k == "--output-fps") { auto v = next(k.c_str()); if (v) output_fps = std::atoi(v); else return 1; }
    else if (k == "--inference-fps") { auto v = next(k.c_str()); if (v) inference_fps = std::atoi(v); else return 1; }
    else if (k == "--no-region-filter") { no_region_filter = true; }
    else if (k == "--streams") { auto v = next(k.c_str()); if (v) streams = v; else return 1; }
    else if (k == "--enable-business") { enable_business = true; }
    else if (k == "--business-socket") { auto v = next(k.c_str()); if (v) business_socket = v; else return 1; }
    else { std::fprintf(stderr, "未知参数: %s\n", k.c_str()); usage(); return 1; }
  }

  bool want_a = (streams.find('A') != std::string::npos);
  bool want_b = (streams.find('B') != std::string::npos);

  std::string a_input, a_output, b_input, b_output;
  if (want_a) {
    if (!env_required("STREAM_A_INPUT_URL", a_input)) return 2;
    if (!env_required("STREAM_A_OUTPUT_URL", a_output)) return 2;
  }
  if (want_b) {
    if (!env_required("STREAM_B_INPUT_URL", b_input)) return 2;
    if (!env_required("STREAM_B_OUTPUT_URL", b_output)) return 2;
  }

  hzw::DualStreamConfig dcfg;
  dcfg.max_seconds = max_seconds;
  dcfg.metrics_interval_sec = metrics_interval;
  dcfg.detector_mode = detector_mode;

  // 默认配置（A 路）
  setup_common(dcfg.stream_a);
  dcfg.stream_a.stream_id = "A";
  dcfg.stream_a.input_path = a_input;
  dcfg.stream_a.output_path = a_output;
  dcfg.stream_a.output_fps = output_fps;
  dcfg.stream_a.inference_fps = inference_fps;
  dcfg.stream_a.jitter_buffer_size = jitter_buffer_size;
  dcfg.stream_a.camera_param = 0.5;
  dcfg.stream_a.enable_business = enable_business;
  dcfg.stream_a.business_socket = business_socket;
  dcfg.stream_a.business_jsonl_path = enable_business ? "artifacts/internal-development/stream_A_events.jsonl" : "";
  if (!no_region_filter) setup_a_forbidden_zones(dcfg.stream_a);

  // 默认配置（B 路）
  setup_common(dcfg.stream_b);
  dcfg.stream_b.stream_id = "B";
  dcfg.stream_b.input_path = b_input;
  dcfg.stream_b.output_path = b_output;
  dcfg.stream_b.output_fps = output_fps;
  dcfg.stream_b.inference_fps = inference_fps;
  dcfg.stream_b.jitter_buffer_size = jitter_buffer_size;
  dcfg.stream_b.camera_param = 0.8;
  dcfg.stream_b.enable_business = enable_business;
  dcfg.stream_b.business_socket = business_socket;
  dcfg.stream_b.business_jsonl_path = enable_business ? "artifacts/internal-development/stream_B_events.jsonl" : "";
  if (!no_region_filter) setup_b_forbidden_zones(dcfg.stream_b);

  hzw::DualStreamApplication app;
  g_app = &app;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  int rc = app.run(dcfg);
  g_app = nullptr;
  return rc;
}
