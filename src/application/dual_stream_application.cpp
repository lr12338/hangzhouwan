// -*- coding: utf-8 -*-
#include "application/dual_stream_application.h"
#include "monitoring/video_health_logic.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace hzw {

namespace {
int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace


namespace {
std::string read_version_file() {
  std::ifstream f("/opt/hangzhouwan/current/VERSION");
  if (!f) return "";
  std::string ver, commit;
  std::getline(f, ver);
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("commit:") == 0) {
      commit = line.substr(7);
      break;
    }
  }
  // Store in a static for reuse
  static std::string s_ver = ver;
  static std::string s_commit = commit;
  s_ver = ver;
  s_commit = commit;
  return ver + "|" + commit;
}
}  // namespace

DualStreamApplication::~DualStreamApplication() {
  request_stop();
  if (t_a_.joinable()) t_a_.join();
  if (t_b_.joinable()) t_b_.join();
  if (t_metrics_.joinable()) t_metrics_.join();
}

void DualStreamApplication::request_stop() {
  bool was = stop_.exchange(true);
  if (!was) {
    pipeline_a_.request_stop();
    pipeline_b_.request_stop();
  }
}

void DualStreamApplication::stream_thread(const PipelineConfig& cfg,
                                          SingleStreamPipeline& pipeline,
                                          std::atomic<int>& exit_code) {
  try {
    exit_code.store(pipeline.run(cfg));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "错误 | [%s] 线程异常 | %s\n",
                 cfg.stream_id.c_str(), e.what());
    std::fflush(stderr);
    exit_code.store(99);
  }
  // 资源致命（退出码 70）：立即停止双路，禁止错误风暴，进程非零退出。
  if (exit_code.load() == 70) {
    std::fprintf(stderr, "致命 | 双路 | [%s] 设备资源致命，停止双路\n", cfg.stream_id.c_str());
    std::fflush(stderr);
    request_stop();
  }
  // 普通错误不直接杀死另一路；由 metrics_loop 的超时或信号统一停止。
}

int DualStreamApplication::run(const DualStreamConfig& cfg) {
  std::fprintf(stdout, "信息 | 双路 | 启动 detector_mode=%s max_seconds=%d\n",
               cfg.detector_mode.c_str(), cfg.max_seconds);
  std::fflush(stdout);

  start_ms_ = now_ms();
  stop_.store(false);
  exit_code_a_.store(0);
  exit_code_b_.store(0);
  prev_output_a_ = prev_infer_a_ = prev_output_b_ = prev_infer_b_ = 0;
  prev_sample_ms_ = start_ms_;

  // 启动 Video 健康 Socket
  std::string ver, commit;
  std::string vc = read_version_file();
  auto sep = vc.find('|');
  if (sep != std::string::npos) { ver = vc.substr(0, sep); commit = vc.substr(sep + 1); }
  {
    VideoHealthState init;
    init.release = ver;
    init.version = ver;
    init.commit = commit;
    init.status = "STARTING";
    health_server_.update_state(init);
  }
  health_server_.start("/run/hangzhouwan/video-health.sock");
  std::fprintf(stdout, "信息 | 健康 | Video 健康 Socket 已启动 /run/hangzhouwan/video-health.sock\n");
  std::fflush(stdout);

  // A/B 按需启动（单路时不启动另一路）
  if (cfg.run_a) {
    t_a_ = std::thread([this, &cfg] { stream_thread(cfg.stream_a, pipeline_a_, exit_code_a_); });
  } else {
    exit_code_a_.store(0);
  }
  if (cfg.run_b) {
    t_b_ = std::thread([this, &cfg] { stream_thread(cfg.stream_b, pipeline_b_, exit_code_b_); });
  } else {
    exit_code_b_.store(0);
  }
  t_metrics_ = std::thread([this, &cfg] { metrics_loop(cfg); });

  if (t_a_.joinable()) t_a_.join();
  if (t_b_.joinable()) t_b_.join();
  stop_.store(true);
  t_metrics_.join();
  health_server_.stop();

  int rc_a = exit_code_a_.load();
  int rc_b = exit_code_b_.load();
  int rc = 0;
  if (cfg.run_a && rc_a == 70) rc = 70;
  if (cfg.run_b && rc_b == 70) rc = 70;
  if (rc != 70) {
    if (cfg.run_a && rc_a != 0) rc = 1;
    if (cfg.run_b && rc_b != 0) rc = 1;
  }
  std::fprintf(stdout, "信息 | 双路 | 结束 A退出码=%d B退出码=%d 耗时=%llds\n",
               rc_a, rc_b, static_cast<long long>((now_ms() - start_ms_) / 1000));
  std::fflush(stdout);
  return rc;
}

void DualStreamApplication::metrics_loop(const DualStreamConfig& cfg) {
  while (!stop_.load()) {
    for (int i = 0; i < cfg.metrics_interval_sec * 10 && !stop_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (stop_.load()) break;

    // 双路汇总
    const auto& ma = pipeline_a_.metrics();
    const auto& mb = pipeline_b_.metrics();
    int64_t now = now_ms();
    int64_t elapsed_ms = now - prev_sample_ms_;
    double elapsed_sec = elapsed_ms / 1000.0;

    // 计算 FPS
    int64_t cur_out_a = ma.output_frames.load();
    int64_t cur_inf_a = ma.inference_count.load();
    int64_t cur_out_b = mb.output_frames.load();
    int64_t cur_inf_b = mb.inference_count.load();
    double fps_out_a = (elapsed_sec > 0) ? (cur_out_a - prev_output_a_) / elapsed_sec : 0;
    double fps_inf_a = (elapsed_sec > 0) ? (cur_inf_a - prev_infer_a_) / elapsed_sec : 0;
    double fps_out_b = (elapsed_sec > 0) ? (cur_out_b - prev_output_b_) / elapsed_sec : 0;
    double fps_inf_b = (elapsed_sec > 0) ? (cur_inf_b - prev_infer_b_) / elapsed_sec : 0;
    prev_output_a_ = cur_out_a;
    prev_infer_a_ = cur_inf_a;
    prev_output_b_ = cur_out_b;
    prev_infer_b_ = cur_inf_b;
    prev_sample_ms_ = now;

    std::fprintf(stdout,
        "信息 | 双路汇总 | A:输出=%lld 推理=%lld 丢帧=%lld  "
        "B:输出=%lld 推理=%lld 丢帧=%lld  "
        "A推理P95=%.0fms B推理P95=%.0fms\n",
        static_cast<long long>(cur_out_a),
        static_cast<long long>(cur_inf_a),
        static_cast<long long>(ma.dropped_frames.load()),
        static_cast<long long>(cur_out_b),
        static_cast<long long>(cur_inf_b),
        static_cast<long long>(mb.dropped_frames.load()),
        0.0, 0.0);  // P95 由各路 summary 输出
    std::fflush(stdout);

    // 更新健康 Socket 状态
    {
      VideoHealthState hs;
      std::string vc = read_version_file();
      auto sep = vc.find('|');
      if (sep != std::string::npos) {
        hs.release = vc.substr(0, sep);
        hs.version = vc.substr(0, sep);
        hs.commit = vc.substr(sep + 1);
      }

      // Business 状态
      auto bs_a = pipeline_a_.business_state();
      std::string biz_state = "DETECTION_ONLY";
      bool business_full = false;
      if (bs_a == EnrichmentState::FULL) { biz_state = "FULL"; business_full = true; }
      else if (bs_a == EnrichmentState::COORD_ONLY) { biz_state = "COORD_ONLY"; business_full = true; }
      else { biz_state = "DETECTION_ONLY"; }
      hs.business_state = biz_state;

      // 单路健康输入：RTSP 断开时长（瞬时/持续分级）、RTMP 输出、FPS、资源致命、重连增量
      HealthThresholds ht;
      const int64_t lr_a = ma.last_read_ms.load();
      const int64_t lr_b = mb.last_read_ms.load();
      // last_read_ms==0（从未读到帧，启动期）视为未断开，避免启动即判死
      int64_t disc_a = (lr_a == 0) ? 0 : (now - lr_a);
      int64_t disc_b = (lr_b == 0) ? 0 : (now - lr_b);
      bool a_rtsp = disc_a < ht.frame_stale_ms;
      bool b_rtsp = disc_b < ht.frame_stale_ms;
      int64_t recon_a = ma.rtsp_reconnects.load() + ma.rtmp_reconnects.load();
      int64_t recon_b = mb.rtsp_reconnects.load() + mb.rtmp_reconnects.load();
      StreamHealthInput in_a;
      in_a.disconnected_ms = disc_a;
      in_a.rtmp_connected = (cur_out_a > prev_output_a_) || (fps_out_a > 0);
      in_a.output_fps = fps_out_a;
      in_a.inference_fps = fps_inf_a;
      in_a.resource_fatal = pipeline_a_.resource_fatal();
      in_a.reconnect_delta = recon_a - prev_reconnect_a_;
      StreamHealthInput in_b;
      in_b.disconnected_ms = disc_b;
      in_b.rtmp_connected = (cur_out_b > prev_output_b_) || (fps_out_b > 0);
      in_b.output_fps = fps_out_b;
      in_b.inference_fps = fps_inf_b;
      in_b.resource_fatal = pipeline_b_.resource_fatal();
      in_b.reconnect_delta = recon_b - prev_reconnect_b_;
      prev_reconnect_a_ = recon_a;
      prev_reconnect_b_ = recon_b;

      // 瞬时等级 -> 防抖（FAILED 立即提交，DEGRADED/恢复需连续确认）-> 整体状态
      StreamHealthLevel inst_a = compute_stream_level(in_a, ht);
      StreamHealthLevel inst_b = compute_stream_level(in_b, ht);
      StreamHealthLevel lvl_a = debouncer_a_.update(inst_a, ht.confirm_down, ht.confirm_up);
      StreamHealthLevel lvl_b = debouncer_b_.update(inst_b, ht.confirm_down, ht.confirm_up);
      DualHealthResult hr = compute_dual_status(lvl_a, lvl_b,
                                                in_a.resource_fatal, in_b.resource_fatal,
                                                business_full);
      hs.status = hr.status;
      hs.degradation = hr.degradation;
      hs.health_reason = hr.reason;
      if (!hr.reason.empty()) {
        std::fprintf(stdout, "信息 | 双路健康 | status=%s degradation=%s (%s) A=%s B=%s\n",
                     hr.status.c_str(), hr.degradation.c_str(), hr.reason.c_str(),
                     level_str(hr.level_a), level_str(hr.level_b));
        std::fflush(stdout);
      }

      // A 路快照
      hs.stream_a.stream_id = "A";
      hs.stream_a.level = level_str(hr.level_a);
      hs.stream_a.rtsp_connected = a_rtsp;
      hs.stream_a.rtmp_connected = cur_out_a > prev_output_a_ || fps_out_a > 0;
      hs.stream_a.output_fps = fps_out_a;
      hs.stream_a.inference_fps = fps_inf_a;
      hs.stream_a.last_frame_time_ms = ma.last_read_ms.load();
      hs.stream_a.rtsp_reconnects = ma.rtsp_reconnects.load();
      hs.stream_a.rtmp_reconnects = ma.rtmp_reconnects.load();
      hs.stream_a.queue_length = pipeline_a_.metrics().queue_length();
      hs.stream_a.e2e_p95_ms = pipeline_a_.metrics().e2e_p95();

      // B 路快照
      hs.stream_b.stream_id = "B";
      hs.stream_b.level = level_str(hr.level_b);
      hs.stream_b.rtsp_connected = b_rtsp;
      hs.stream_b.rtmp_connected = cur_out_b > prev_output_b_ || fps_out_b > 0;
      hs.stream_b.output_fps = fps_out_b;
      hs.stream_b.inference_fps = fps_inf_b;
      hs.stream_b.last_frame_time_ms = mb.last_read_ms.load();
      hs.stream_b.rtsp_reconnects = mb.rtsp_reconnects.load();
      hs.stream_b.rtmp_reconnects = mb.rtmp_reconnects.load();
      hs.stream_b.queue_length = pipeline_b_.metrics().queue_length();
      hs.stream_b.e2e_p95_ms = pipeline_b_.metrics().e2e_p95();

      hs.uptime_seconds = (now - start_ms_) / 1000;
      health_server_.update_state(hs);
    }

    if (cfg.max_seconds > 0 && (now_ms() - start_ms_) / 1000 >= cfg.max_seconds) {
      std::fprintf(stdout, "信息 | 双路 | 达到 %d 秒，主动停止\n", cfg.max_seconds);
      std::fflush(stdout);
      request_stop();
      break;
    }
  }
}

}  // namespace hzw
