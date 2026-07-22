// -*- coding: utf-8 -*-
// =============================================================================
// 双路并发视频推理 CLI（生产收口版）。
//
// 配置驱动：所有参数从 /etc/hangzhouwan/application.yaml 读取。
// 真实 RTSP/RTMP 地址从环境变量（由配置指定变量名）注入，不入命令行/日志。
// 业务增强通过 --enable-business 启用，socket 路径由 --business-socket 或配置指定。
//
// 用法：
//   dual_stream_app --config /etc/hangzhouwan/application.yaml \
//                   --enable-business \
//                   --business-socket /run/hangzhouwan/business.sock
//
// 测试选项（不影响生产配置）：
//   --max-seconds 300       最长运行秒数（0=不限）
//   --metrics-interval 10   指标输出间隔秒
//   --streams A,B           指定启动的流（默认全部 enabled 流）
//   --no-region-filter      临时禁用禁区过滤
// =============================================================================
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "application/dual_stream_application.h"
#include "config/application_config.h"

namespace {

hzw::DualStreamApplication* g_app = nullptr;

void on_signal(int sig) {
  if (g_app) {
    std::fprintf(stdout, "\n信息 | 信号 | 收到信号 %d，停止双路\n", sig);
    std::fflush(stdout);
    g_app->request_stop();
  }
}

void usage() {
  std::fprintf(stdout,
      "用法: dual_stream_app [选项]\n"
      "  --config PATH           配置文件路径（必需，默认 /etc/hangzhouwan/application.yaml）\n"
      "  --enable-business       启用业务增强（坐标+AIS）\n"
      "  --business-socket PATH  Sidecar Unix Socket 路径\n"
      "  --max-seconds 300       最长运行秒数（0=不限）\n"
      "  --metrics-interval 10   指标输出间隔秒\n"
      "  --streams A,B           指定启动的流（默认全部 enabled 流）\n"
      "  --no-region-filter      临时禁用禁区过滤\n"
      "\n"
      "环境变量（由配置文件指定变量名）：\n"
      "  RTSP 输入 / RTMP 输出 URL 通过环境变量注入\n");
}

// 从 ApplicationConfig + StreamConfig 构建 PipelineConfig。
hzw::PipelineConfig build_pipeline_config(const hzw::ApplicationConfig& app_cfg,
                                          const hzw::StreamConfig& sc,
                                          bool no_region_filter,
                                          const std::string& business_socket,
                                          bool enable_business,
                                          const std::string& jsonl_path) {
  hzw::PipelineConfig pc;
  pc.stream_id = sc.id;
  pc.input_path = app_cfg.resolve_env(sc.input_url_env);
  pc.output_path = app_cfg.resolve_env(sc.output_url_env);
  pc.bmodel_path = app_cfg.bmodel_path;
  pc.device = app_cfg.device;
  pc.decoder = app_cfg.decoder;
  pc.encoder = app_cfg.encoder;
  pc.source_fps = 25;  // RTSP 模式不依赖源帧率整除
  pc.output_fps = sc.output_fps;
  pc.inference_fps = sc.inference_fps;
  pc.bitrate_kbps = sc.bitrate_kbps;
  pc.gop = sc.gop;
  pc.queue_size = app_cfg.frame_queue_size;
  pc.conf = sc.conf;
  pc.iou = sc.iou;
  pc.result_ttl_ms = sc.result_ttl_ms;
  pc.preprocess = app_cfg.preprocess;
  pc.draw_mode = app_cfg.draw_mode;
  pc.source_type = "rtsp";
  pc.rtsp_transport = "tcp";
  pc.rtsp_stimeout_us = 5000000;
  pc.rtsp_max_reconnect = -1;
  pc.rtsp_initial_backoff_ms = app_cfg.reconnect_initial_seconds * 1000;
  pc.rtsp_max_backoff_ms = app_cfg.reconnect_max_seconds * 1000;
  pc.sink_type = "rtmp";
  pc.jitter_buffer_size = sc.jitter_buffer_size;
  pc.metrics_interval_sec = app_cfg.health_check_interval_seconds;
  pc.coordinate_model_path = sc.coordinate_model_path;
  pc.coordinate_mode = app_cfg.coordinate_mode;
  pc.camera_param = sc.camera_param;
  pc.enable_business = enable_business;
  pc.business_socket = business_socket;
  pc.business_jsonl_path = enable_business ? jsonl_path : "";
  pc.request_timeout_ms = app_cfg.request_timeout_ms;

  if (!no_region_filter && (!sc.forbidden_rectangles.empty() || !sc.forbidden_polygons.empty())) {
    pc.enable_region_filter = true;
    pc.region_ref_width = app_cfg.reference_width;
    pc.region_ref_height = app_cfg.reference_height;
    pc.forbidden_rectangles = sc.forbidden_rectangles;
    pc.forbidden_polygons = sc.forbidden_polygons;
  }
  return pc;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "/etc/hangzhouwan/application.yaml";
  int max_seconds = 0;
  int metrics_interval = 10;
  std::string streams_filter;
  bool no_region_filter = false;
  bool enable_business = false;
  std::string business_socket;

  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "缺少 %s 的值\n", name); return nullptr; }
      return argv[++i];
    };
    if (k == "--help" || k == "-h") { usage(); return 0; }
    else if (k == "--config") { auto v = next(k.c_str()); if (v) config_path = v; else return 1; }
    else if (k == "--max-seconds") { auto v = next(k.c_str()); if (v) max_seconds = std::atoi(v); else return 1; }
    else if (k == "--metrics-interval") { auto v = next(k.c_str()); if (v) metrics_interval = std::atoi(v); else return 1; }
    else if (k == "--streams") { auto v = next(k.c_str()); if (v) streams_filter = v; else return 1; }
    else if (k == "--no-region-filter") { no_region_filter = true; }
    else if (k == "--enable-business") { enable_business = true; }
    else if (k == "--business-socket") { auto v = next(k.c_str()); if (v) business_socket = v; else return 1; }
    else { std::fprintf(stderr, "未知参数: %s\n", k.c_str()); usage(); return 1; }
  }

  // 加载配置
  hzw::ApplicationConfig app_cfg;
  std::string cfg_err;
  if (!app_cfg.load(config_path, cfg_err)) {
    std::fprintf(stderr, "致命 | 配置 | 加载失败: %s\n", cfg_err.c_str());
    return 3;
  }
  std::fprintf(stdout, "信息 | 配置 | 已加载 %s environment=%s bmodel=%s coordinate_mode=%s\n",
               config_path.c_str(), app_cfg.environment.c_str(),
               app_cfg.bmodel_path.c_str(), app_cfg.coordinate_mode.c_str());
  std::fflush(stdout);

  // business socket 默认从配置取
  if (business_socket.empty()) {
    business_socket = app_cfg.business_socket;
  }

  // 构建 PipelineConfig
  std::vector<hzw::PipelineConfig> pipeline_cfgs;
  for (const auto& sc : app_cfg.streams) {
    if (!sc.enabled) continue;
    if (!streams_filter.empty() && streams_filter.find(sc.id) == std::string::npos) continue;
    std::string input_url = app_cfg.resolve_env(sc.input_url_env);
    std::string output_url = app_cfg.resolve_env(sc.output_url_env);
    if (input_url.empty()) {
      std::fprintf(stderr, "错误 | 配置 | 流 %s 环境变量 %s 未设置\n",
                   sc.id.c_str(), sc.input_url_env.c_str());
      return 2;
    }
    if (output_url.empty()) {
      std::fprintf(stderr, "错误 | 配置 | 流 %s 环境变量 %s 未设置\n",
                   sc.id.c_str(), sc.output_url_env.c_str());
      return 2;
    }
    std::string jsonl_path = "/var/lib/hangzhouwan/stream_" + sc.id + "_events.jsonl";
    pipeline_cfgs.push_back(build_pipeline_config(app_cfg, sc, no_region_filter,
                                                  business_socket, enable_business, jsonl_path));
  }

  if (pipeline_cfgs.empty()) {
    std::fprintf(stderr, "致命 | 配置 | 没有启用的流\n");
    return 4;
  }

  hzw::DualStreamConfig dcfg;
  dcfg.max_seconds = max_seconds;
  dcfg.metrics_interval_sec = metrics_interval;
  dcfg.detector_mode = "per_stream";

  dcfg.run_a = false;
  dcfg.run_b = false;
  if (pipeline_cfgs.size() >= 1) { dcfg.stream_a = pipeline_cfgs[0]; dcfg.run_a = true; }
  if (pipeline_cfgs.size() >= 2) { dcfg.stream_b = pipeline_cfgs[1]; dcfg.run_b = true; }

  hzw::DualStreamApplication app;
  g_app = &app;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  int rc = app.run(dcfg);
  g_app = nullptr;
  return rc;
}
