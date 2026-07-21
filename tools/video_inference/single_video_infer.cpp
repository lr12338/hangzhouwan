// -*- coding: utf-8 -*-
// =============================================================================
// 阶段4 单路视频硬件推理 CLI。
//
// 链路：本地 mp4 -> h264_bm 硬解 -> 容量1丢旧队列 -> 间隔推理(复用snapshot) -> 绘框
//       -> in-place 写回 NV12 -> h264_bm 硬编 -> 本地输出文件。
//
// 用法见 tools/video_inference/README.md。响应 SIGINT/SIGTERM 优雅退出。
// =============================================================================
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#include "pipeline/single_stream_pipeline.h"

namespace {
hzw::SingleStreamPipeline* g_pipeline = nullptr;

bool ensure_dir(const std::string& path) {
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return true;
  std::string dir = path.substr(0, slash);
  if (dir.empty()) return true;
  std::string acc;
  for (size_t i = 0; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == '/') {
      if (!acc.empty()) mkdir(acc.c_str(), 0775);
    }
    if (i < dir.size()) acc += dir[i];
  }
  return true;
}

struct Args {
  std::string input = "testdata/test.mp4";
  std::string output = "artifacts/stage4/output_single_stream.mp4";
  std::string bmodel = "artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel";
  int device = 0;
  std::string decoder = "h264_bm";
  std::string encoder = "h264_bm";
  int source_fps = 20;
  int output_fps = 10;
  int inference_fps = 5;
  int bitrate_kbps = 800;
  int gop = 20;
  int queue_size = 1;
  float conf = 0.1f;
  float iou = 0.1f;
  int result_ttl_ms = 1000;
  int loop = 1;
  int max_seconds = 0;
  int metrics_interval_sec = 10;
  std::string preprocess = "cpu";
  std::string draw_mode = "cpu";
  bool help = false;
};

bool parse(int argc, char** argv, Args& a, std::string& err) {
  auto get = [&](const char* k, int& i) -> const char* {
    if (i + 1 >= argc) { err = std::string("缺少 ") + k + " 的值"; return nullptr; }
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    const char* v = nullptr;
    if (k == "--help" || k == "-h") { a.help = true; return true; }
    else if (k == "--input") { v = get(k.c_str(), i); if (v) a.input = v; else return false; }
    else if (k == "--output") { v = get(k.c_str(), i); if (v) a.output = v; else return false; }
    else if (k == "--bmodel") { v = get(k.c_str(), i); if (v) a.bmodel = v; else return false; }
    else if (k == "--device") { v = get(k.c_str(), i); if (!v) return false; a.device = std::atoi(v); }
    else if (k == "--decoder") { v = get(k.c_str(), i); if (v) a.decoder = v; else return false; }
    else if (k == "--encoder") { v = get(k.c_str(), i); if (v) a.encoder = v; else return false; }
    else if (k == "--source-fps") { v = get(k.c_str(), i); if (!v) return false; a.source_fps = std::atoi(v); }
    else if (k == "--output-fps") { v = get(k.c_str(), i); if (!v) return false; a.output_fps = std::atoi(v); }
    else if (k == "--inference-fps") { v = get(k.c_str(), i); if (!v) return false; a.inference_fps = std::atoi(v); }
    else if (k == "--bitrate-kbps") { v = get(k.c_str(), i); if (!v) return false; a.bitrate_kbps = std::atoi(v); }
    else if (k == "--gop") { v = get(k.c_str(), i); if (!v) return false; a.gop = std::atoi(v); }
    else if (k == "--queue-size") { v = get(k.c_str(), i); if (!v) return false; a.queue_size = std::atoi(v); }
    else if (k == "--conf") { v = get(k.c_str(), i); if (!v) return false; a.conf = static_cast<float>(std::atof(v)); }
    else if (k == "--iou") { v = get(k.c_str(), i); if (!v) return false; a.iou = static_cast<float>(std::atof(v)); }
    else if (k == "--result-ttl-ms") { v = get(k.c_str(), i); if (!v) return false; a.result_ttl_ms = std::atoi(v); }
    else if (k == "--loop") { v = get(k.c_str(), i); if (!v) return false; a.loop = std::atoi(v); }
    else if (k == "--max-seconds") { v = get(k.c_str(), i); if (!v) return false; a.max_seconds = std::atoi(v); }
    else if (k == "--metrics-interval") { v = get(k.c_str(), i); if (!v) return false; a.metrics_interval_sec = std::atoi(v); }
    else if (k == "--preprocess") { v = get(k.c_str(), i); if (v) a.preprocess = v; else return false; }
    else if (k == "--draw-mode") { v = get(k.c_str(), i); if (v) a.draw_mode = v; else return false; }
    else { err = "未知参数: " + k; return false; }
  }
  return true;
}

void usage() {
  std::fprintf(stdout,
      "用法: single_video_infer --input <mp4> --output <mp4|ts|h264> --bmodel <bmodel>\n"
      "  --device 0 --decoder h264_bm --encoder h264_bm\n"
      "  --source-fps 20 --output-fps 10 --inference-fps 5\n"
      "  --bitrate-kbps 800 --gop 20 --queue-size 1\n"
      "  --conf 0.1 --iou 0.1 --result-ttl-ms 1000\n"
      "  --loop 1 (0=无限) --max-seconds 0 (0=不限时) --metrics-interval 10\n"
      "  --preprocess cpu|bmcv --draw-mode cpu|bmcv|none\n");
}
}  // namespace

#ifdef __cplusplus
extern "C" {
#endif
static void on_signal(int sig) {
  if (g_pipeline) {
    std::fprintf(stdout, "\n信息 | 信号 | 收到信号 %d，请求停止\n", sig);
    std::fflush(stdout);
    g_pipeline->request_stop();
  }
}
#ifdef __cplusplus
}
#endif

int main(int argc, char** argv) {
  Args a;
  std::string err;
  if (!parse(argc, argv, a, err)) {
    std::fprintf(stderr, "错误 | 参数 | %s\n", err.c_str());
    usage();
    return 1;
  }
  if (a.help) { usage(); return 0; }

  hzw::PipelineConfig cfg;
  cfg.input_path = a.input;
  cfg.output_path = a.output;
  cfg.bmodel_path = a.bmodel;
  cfg.device = a.device;
  cfg.decoder = a.decoder;
  cfg.encoder = a.encoder;
  cfg.source_fps = a.source_fps;
  cfg.output_fps = a.output_fps;
  cfg.inference_fps = a.inference_fps;
  cfg.bitrate_kbps = a.bitrate_kbps;
  cfg.gop = a.gop;
  cfg.queue_size = a.queue_size;
  cfg.conf = a.conf;
  cfg.iou = a.iou;
  cfg.result_ttl_ms = a.result_ttl_ms;
  cfg.loop = a.loop;
  cfg.max_seconds = a.max_seconds;
  cfg.metrics_interval_sec = a.metrics_interval_sec;
  cfg.preprocess = a.preprocess;
  cfg.draw_mode = a.draw_mode;

  ensure_dir(cfg.output_path);

  hzw::SingleStreamPipeline pipeline;
  g_pipeline = &pipeline;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  int rc = pipeline.run(cfg);
  g_pipeline = nullptr;
  return rc;
}
