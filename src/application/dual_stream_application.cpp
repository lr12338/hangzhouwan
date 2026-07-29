// -*- coding: utf-8 -*-
#include "application/dual_stream_application.h"
#include "monitoring/video_health_logic.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <unistd.h>

namespace hzw {

namespace {
int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

const char* lifecycle_str(PipelineLifecycle lifecycle) {
  switch (lifecycle) {
    case PipelineLifecycle::STARTING: return "STARTING";
    case PipelineLifecycle::RUNNING: return "RUNNING";
    case PipelineLifecycle::EXITED: return "EXITED";
  }
  return "EXITED";
}

bool probe_data_storage(const std::string& /*path*/, int64_t& free_bytes,
                        std::string& error) {
  struct stat root {};
  struct stat data {};
  struct statvfs fs {};
  if (::stat("/", &root) != 0 || ::stat("/data", &data) != 0 ||
      !S_ISDIR(data.st_mode) || root.st_dev == data.st_dev) {
    error = "/data 不是独立挂载点";
    return false;
  }
  if (::statvfs("/data", &fs) != 0) {
    error = "无法读取事件存储空间";
    return false;
  }
  free_bytes = static_cast<int64_t>(fs.f_bavail) *
               static_cast<int64_t>(fs.f_frsize);
  return true;
}

std::string json_string_value(const std::string& json,
                              const std::string& key) {
  const std::string marker = "\"" + key + "\"";
  size_t pos = json.find(marker);
  if (pos == std::string::npos) return "";
  pos = json.find(':', pos + marker.size());
  if (pos == std::string::npos) return "";
  pos = json.find('"', pos + 1);
  if (pos == std::string::npos) return "";
  const size_t end = json.find('"', pos + 1);
  return end == std::string::npos ? "" : json.substr(pos + 1, end - pos - 1);
}

bool query_sidecar_health(const std::string& socket_path) {
  if (socket_path.empty()) return false;
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  struct timeval timeout {0, 200000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_un address {};
  address.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(address.sun_path)) {
    ::close(fd);
    return false;
  }
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&address),
                sizeof(address)) != 0) {
    ::close(fd);
    return false;
  }
  static const char request[] = "{\"action\":\"health\"}\n";
  if (::send(fd, request, sizeof(request) - 1, MSG_NOSIGNAL) !=
      static_cast<ssize_t>(sizeof(request) - 1)) {
    ::close(fd);
    return false;
  }
  std::string response;
  char buffer[4096];
  while (response.find('\n') == std::string::npos &&
         response.size() < 65536) {
    const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
    if (count <= 0) break;
    response.append(buffer, static_cast<size_t>(count));
  }
  ::close(fd);
  return json_string_value(response, "status") == "HEALTHY";
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
    exit_code.store(kPipelineExitRuntime);
  }
  // 任一已启用流非零退出都停止双路，避免主进程保持 active 而数据面残缺。
  if (exit_code.load() != 0) {
    std::fprintf(stderr, "错误 | 双路 | [%s] 管线退出码=%d，停止双路\n",
                 cfg.stream_id.c_str(), exit_code.load());
    std::fflush(stderr);
    request_stop();
  }
}

int DualStreamApplication::run(const DualStreamConfig& cfg) {
  std::fprintf(stdout, "信息 | 双路 | 启动 detector_mode=%s max_seconds=%d\n",
               cfg.detector_mode.c_str(), cfg.max_seconds);
  std::fflush(stdout);

  start_ms_ = now_ms();
  stop_.store(false);
  exit_code_a_.store(cfg.run_a ? -1 : 0);
  exit_code_b_.store(cfg.run_b ? -1 : 0);
  prev_output_a_ = prev_infer_a_ = prev_output_b_ = prev_infer_b_ = 0;
  prev_reconnect_a_ = prev_reconnect_b_ = 0;
  reconnect_times_a_.clear();
  reconnect_times_b_.clear();
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
  if ((cfg.run_a && rc_a == kPipelineExitHardware) ||
      (cfg.run_b && rc_b == kPipelineExitHardware)) {
    rc = kPipelineExitHardware;
  } else if ((cfg.run_a && rc_a == kPipelineExitEndpointUnavailable) ||
             (cfg.run_b && rc_b == kPipelineExitEndpointUnavailable)) {
    rc = kPipelineExitEndpointUnavailable;
  } else if ((cfg.run_a && rc_a == kPipelineExitConfiguration) ||
             (cfg.run_b && rc_b == kPipelineExitConfiguration)) {
    rc = kPipelineExitConfiguration;
  } else if ((cfg.run_a && rc_a != 0) || (cfg.run_b && rc_b != 0)) {
    rc = kPipelineExitRuntime;
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
    const bool output_progress_a = cur_out_a > prev_output_a_;
    const bool output_progress_b = cur_out_b > prev_output_b_;
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

      const PipelineLifecycle life_a = pipeline_a_.lifecycle();
      const PipelineLifecycle life_b = pipeline_b_.lifecycle();

      // Business 链路与流初始化解耦：未进入 RUNNING 的管线不伪造 Sidecar 故障。
      auto bs_a = pipeline_a_.business_state();
      auto bs_b = pipeline_b_.business_state();
      const bool enabled_a = cfg.run_a && cfg.stream_a.enable_business;
      const bool enabled_b = cfg.run_b && cfg.stream_b.enable_business;
      const std::string& sidecar_socket =
          enabled_a ? cfg.stream_a.business_socket : cfg.stream_b.business_socket;
      const bool sidecar_healthy =
          !(enabled_a || enabled_b) || query_sidecar_health(sidecar_socket);
      const bool participant_a =
          enabled_a && life_a == PipelineLifecycle::RUNNING;
      const bool participant_b =
          enabled_b && life_b == PipelineLifecycle::RUNNING;
      BusinessHealthResult business = compute_business_health(
          participant_a,
          pipeline_a_.business_link_healthy() && sidecar_healthy,
          enrichment_status_str(bs_a),
          participant_b,
          pipeline_b_.business_link_healthy() && sidecar_healthy,
          enrichment_status_str(bs_b));
      if ((enabled_a || enabled_b) && !participant_a && !participant_b) {
        business.link_healthy = sidecar_healthy;
        business.link = sidecar_healthy ? "HEALTHY" : "FAILED";
        business.mode = sidecar_healthy ? "PENDING" : "DETECTION_ONLY";
      }
      const bool business_link_healthy = business.link_healthy;
      hs.business_link = business.link;
      hs.enrichment_mode = business.mode;
      // 旧字段保留一个 Release 周期。
      hs.business_state = hs.enrichment_mode;
      hs.business_timeout_count =
          pipeline_a_.business_timeout_count() + pipeline_b_.business_timeout_count();
      hs.business_error_count =
          pipeline_a_.business_error_count() + pipeline_b_.business_error_count();

      // 单路健康输入：RTSP 断开时长（瞬时/持续分级）、RTMP 输出、FPS、资源致命、重连增量
      HealthThresholds ht = cfg.health_thresholds;
      const int64_t lr_a = ma.last_read_ms.load();
      const int64_t lr_b = mb.last_read_ms.load();
      // 从未读到首帧时，以应用启动时间计算有限宽限，禁止永久 STARTING。
      int64_t disc_a = (lr_a == 0) ? (now - start_ms_) : (now - lr_a);
      int64_t disc_b = (lr_b == 0) ? (now - start_ms_) : (now - lr_b);
      bool a_rtsp = lr_a > 0 && disc_a < ht.frame_stale_ms;
      bool b_rtsp = lr_b > 0 && disc_b < ht.frame_stale_ms;
      int64_t recon_a = ma.rtsp_reconnect_attempts.load() +
                        ma.rtmp_reconnect_attempts.load();
      int64_t recon_b = mb.rtsp_reconnect_attempts.load() +
                        mb.rtmp_reconnect_attempts.load();
      update_reconnect_attempt_window(
          reconnect_times_a_, recon_a, prev_reconnect_a_, now);
      update_reconnect_attempt_window(
          reconnect_times_b_, recon_b, prev_reconnect_b_, now);
      StreamHealthInput in_a;
      in_a.disconnected_ms = disc_a;
      in_a.starting = cfg.run_a && lr_a == 0 &&
                      life_a != PipelineLifecycle::EXITED &&
                      disc_a < ht.frame_stale_ms;
      in_a.thread_failed =
          cfg.run_a && life_a == PipelineLifecycle::EXITED &&
          pipeline_a_.pipeline_exit_code() != 0;
      in_a.rtmp_connected = output_progress_a || (fps_out_a > 0);
      in_a.output_fps = fps_out_a;
      in_a.inference_fps = fps_inf_a;
      in_a.resource_fatal = pipeline_a_.resource_fatal();
      in_a.reconnects_1h = reconnect_times_a_.size();
      StreamHealthInput in_b;
      in_b.disconnected_ms = disc_b;
      in_b.starting = cfg.run_b && lr_b == 0 &&
                      life_b != PipelineLifecycle::EXITED &&
                      disc_b < ht.frame_stale_ms;
      in_b.thread_failed =
          cfg.run_b && life_b == PipelineLifecycle::EXITED &&
          pipeline_b_.pipeline_exit_code() != 0;
      in_b.rtmp_connected = output_progress_b || (fps_out_b > 0);
      in_b.output_fps = fps_out_b;
      in_b.inference_fps = fps_inf_b;
      in_b.resource_fatal = pipeline_b_.resource_fatal();
      in_b.reconnects_1h = reconnect_times_b_.size();
      prev_reconnect_a_ = recon_a;
      prev_reconnect_b_ = recon_b;

      // 瞬时等级 -> 防抖（FAILED 立即提交，DEGRADED/恢复需连续确认）-> 整体状态
      StreamHealthLevel inst_a = compute_stream_level(in_a, ht);
      StreamHealthLevel inst_b = compute_stream_level(in_b, ht);
      StreamHealthLevel lvl_a = debouncer_a_.update(inst_a, ht.confirm_down, ht.confirm_up);
      StreamHealthLevel lvl_b = debouncer_b_.update(inst_b, ht.confirm_down, ht.confirm_up);
      DualHealthResult hr = compute_dual_status(lvl_a, lvl_b,
                                                in_a.resource_fatal, in_b.resource_fatal,
                                                business_link_healthy,
                                                cfg.run_a, cfg.run_b);
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
      hs.stream_a.level = cfg.run_a ? level_str(hr.level_a) : "DISABLED";
      hs.stream_a.lifecycle =
          cfg.run_a ? lifecycle_str(life_a) : "DISABLED";
      hs.stream_a.exit_code =
          cfg.run_a ? pipeline_a_.pipeline_exit_code() : 0;
      hs.stream_a.failure_stage = pipeline_a_.failure_stage();
      hs.stream_a.last_error = pipeline_a_.last_error();
      hs.stream_a.rtsp_connected = a_rtsp;
      hs.stream_a.rtmp_connected = output_progress_a || fps_out_a > 0;
      hs.stream_a.output_fps = fps_out_a;
      hs.stream_a.inference_fps = fps_inf_a;
      hs.stream_a.last_frame_time_ms = ma.last_read_ms.load();
      hs.stream_a.rtsp_reconnects = ma.rtsp_reconnects.load();
      hs.stream_a.rtmp_reconnects = ma.rtmp_reconnects.load();
      hs.stream_a.reconnects_1h = reconnect_times_a_.size();
      hs.stream_a.rtsp_reconnect_attempts =
          ma.rtsp_reconnect_attempts.load();
      hs.stream_a.rtsp_reconnect_failures =
          ma.rtsp_reconnect_failures.load();
      hs.stream_a.rtmp_reconnect_attempts =
          ma.rtmp_reconnect_attempts.load();
      hs.stream_a.rtmp_reconnect_failures =
          ma.rtmp_reconnect_failures.load();
      hs.stream_a.reconnect_attempts_1h = reconnect_times_a_.size();
      hs.stream_a.queue_length = pipeline_a_.metrics().queue_length();
      hs.stream_a.decode_queue_dropped = ma.decode_queue_dropped.load();
      hs.stream_a.process_queue_dropped = ma.dropped_frames.load();
      hs.stream_a.source_fps = pipeline_a_.source_fps();
      hs.stream_a.input_interarrival_p95_ms =
          ma.interarrival_p95();
      hs.stream_a.input_interarrival_p99_ms =
          ma.interarrival_p99();
      hs.stream_a.e2e_p95_ms = pipeline_a_.metrics().e2e_p95();

      // B 路快照
      hs.stream_b.stream_id = "B";
      hs.stream_b.level = cfg.run_b ? level_str(hr.level_b) : "DISABLED";
      hs.stream_b.lifecycle =
          cfg.run_b ? lifecycle_str(life_b) : "DISABLED";
      hs.stream_b.exit_code =
          cfg.run_b ? pipeline_b_.pipeline_exit_code() : 0;
      hs.stream_b.failure_stage = pipeline_b_.failure_stage();
      hs.stream_b.last_error = pipeline_b_.last_error();
      hs.stream_b.rtsp_connected = b_rtsp;
      hs.stream_b.rtmp_connected = output_progress_b || fps_out_b > 0;
      hs.stream_b.output_fps = fps_out_b;
      hs.stream_b.inference_fps = fps_inf_b;
      hs.stream_b.last_frame_time_ms = mb.last_read_ms.load();
      hs.stream_b.rtsp_reconnects = mb.rtsp_reconnects.load();
      hs.stream_b.rtmp_reconnects = mb.rtmp_reconnects.load();
      hs.stream_b.reconnects_1h = reconnect_times_b_.size();
      hs.stream_b.rtsp_reconnect_attempts =
          mb.rtsp_reconnect_attempts.load();
      hs.stream_b.rtsp_reconnect_failures =
          mb.rtsp_reconnect_failures.load();
      hs.stream_b.rtmp_reconnect_attempts =
          mb.rtmp_reconnect_attempts.load();
      hs.stream_b.rtmp_reconnect_failures =
          mb.rtmp_reconnect_failures.load();
      hs.stream_b.reconnect_attempts_1h = reconnect_times_b_.size();
      hs.stream_b.queue_length = pipeline_b_.metrics().queue_length();
      hs.stream_b.decode_queue_dropped = mb.decode_queue_dropped.load();
      hs.stream_b.process_queue_dropped = mb.dropped_frames.load();
      hs.stream_b.source_fps = pipeline_b_.source_fps();
      hs.stream_b.input_interarrival_p95_ms =
          mb.interarrival_p95();
      hs.stream_b.input_interarrival_p99_ms =
          mb.interarrival_p99();
      hs.stream_b.e2e_p95_ms = pipeline_b_.metrics().e2e_p95();

      const auto ew_a = pipeline_a_.event_writer_status();
      const auto ew_b = pipeline_b_.event_writer_status();
      auto copy_writer = [](const AsyncJsonlWriterStatus& src,
                            EventWriterHealthSnapshot& dst) {
        dst.running = src.running;
        dst.write_enabled = src.write_enabled;
        dst.low_space_warning = src.low_space_warning;
        dst.queued_bytes = src.queued_bytes;
        dst.written_records = src.written_records;
        dst.dropped_records = src.dropped_records;
        dst.last_error = src.last_error;
      };
      copy_writer(ew_a, hs.event_writer_a);
      copy_writer(ew_b, hs.event_writer_b);
      hs.event_writer_a.state = compute_event_writer_state(
          cfg.run_a && cfg.stream_a.enable_business &&
              life_a == PipelineLifecycle::RUNNING,
          ew_a.running, ew_a.write_enabled, !ew_a.last_error.empty());
      hs.event_writer_b.state = compute_event_writer_state(
          cfg.run_b && cfg.stream_b.enable_business &&
              life_b == PipelineLifecycle::RUNNING,
          ew_b.running, ew_b.write_enabled, !ew_b.last_error.empty());
      hs.storage.path = cfg.run_a ? cfg.stream_a.event_directory
                                  : cfg.stream_b.event_directory;
      std::string storage_error;
      hs.storage.available = probe_data_storage(
          hs.storage.path, hs.storage.free_bytes, storage_error);
      const int warn_mb = cfg.run_a ? cfg.stream_a.event_disk_warn_mb
                                    : cfg.stream_b.event_disk_warn_mb;
      const int stop_mb = cfg.run_a ? cfg.stream_a.event_disk_stop_mb
                                    : cfg.stream_b.event_disk_stop_mb;
      hs.storage.state = compute_storage_state(
          hs.storage.available, hs.storage.free_bytes,
          static_cast<int64_t>(warn_mb) * 1024 * 1024,
          static_cast<int64_t>(stop_mb) * 1024 * 1024);
      hs.resource_fatal = in_a.resource_fatal || in_b.resource_fatal;
      if (hs.storage.state != "OK")
        hs.active_alerts.push_back("event_storage_" + hs.storage.state);
      if (!business_link_healthy) hs.active_alerts.push_back("business_link_failed");
      if (hs.business_timeout_count > 0)
        hs.active_alerts.push_back("business_timeout");
      if (hs.business_error_count > 0)
        hs.active_alerts.push_back("business_error");
      if (ew_a.dropped_records > 0 || ew_b.dropped_records > 0)
        hs.active_alerts.push_back("event_writer_dropped");
      if (hs.event_writer_a.state == "FAILED")
        hs.active_alerts.push_back("event_writer_A_failed");
      if (hs.event_writer_b.state == "FAILED")
        hs.active_alerts.push_back("event_writer_B_failed");
      if (hs.resource_fatal)
        hs.active_alerts.push_back("device_resource_fatal");
      if (in_a.reconnects_1h > ht.max_reconnects_per_hour)
        hs.active_alerts.push_back("stream_A_reconnect_rate");
      if (in_b.reconnects_1h > ht.max_reconnects_per_hour)
        hs.active_alerts.push_back("stream_B_reconnect_rate");

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
