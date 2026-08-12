// -*- coding: utf-8 -*-
// CaptureEngine 实现：复用 hzw_inf 组件，事件驱动抓拍。
#include "capture/capture_engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include "capture/capture_core.h"
#include "capture/capture_protocol.h"
#include "inference/yolov7_postprocess.h"
#include "image_io/jpeg_io.h"
#include "video/rtsp_source_options.h"

namespace hzw {

namespace {
int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace

CaptureEngine::CaptureEngine() = default;
CaptureEngine::~CaptureEngine() { stop(); }

bool CaptureEngine::init(const CaptureEngineConfig& cfg, std::string& err) {
  cfg_ = cfg;
  detector_ = std::make_unique<BmrtDetector>(cfg.device, cfg.bmodel_path);
  if (!detector_ || !detector_->ok()) {
    err = detector_ ? detector_->last_error() : "BmrtDetector 分配失败";
    resource_fatal_.store(true);
    return false;
  }
  // BmcvProcessor 延迟到首个会话按实际分辨率 init（分辨率随摄像头不同）。
  start_ms_.store(wall_ms());
  north_ = std::make_unique<Session>();
  south_ = std::make_unique<Session>();
  std::fprintf(stdout,
      "信息 | capture | 模型加载成功 net=%s in=%s out=%s\n",
      detector_->net_name().c_str(), detector_->input_name().c_str(),
      detector_->output_name().c_str());
  std::fflush(stdout);
  return true;
}

const BridgeCaptureConfig* CaptureEngine::bridge_cfg(const std::string& bridge) const {
  if (bridge == "north") return &cfg_.north;
  if (bridge == "south") return &cfg_.south;
  return nullptr;
}

std::string CaptureEngine::handle_arm(const CaptureArmRequest& req) {
  if (resource_fatal_.load()) {
    return make_arm_response(false, req.session_id, "RESOURCE_FATAL",
                             "device resource fatal");
  }
  const BridgeCaptureConfig* bc = bridge_cfg(req.bridge);
  if (!bc) {
    return make_arm_response(false, req.session_id, "FAILED", "unknown bridge");
  }
  Session* s = (req.bridge == "north") ? north_.get() : south_.get();
  std::lock_guard<std::mutex> lk(s->mtx);
  if (s->state.load() != CaptureState::IDLE) {
    return make_arm_response(false, req.session_id, capture_state_name(s->state.load()),
                             "bridge busy");
  }
  if (s->worker.joinable()) s->worker.join();
  s->req = req;
  s->cancel.store(false);
  s->state.store(CaptureState::OPENING_STREAM);
  s->worker = std::thread(&CaptureEngine::run_session, this, std::ref(*s), req);
  return make_arm_response(true, req.session_id, "QUEUED");
}

std::string CaptureEngine::handle_status() const {
  auto snapshot = [this](const std::unique_ptr<Session>& s) {
    CaptureState st = s ? s->state.load() : CaptureState::IDLE;
    std::string sid = s ? s->req.session_id : "";
    std::string jpeg = s ? s->last_jpeg_path : "";
    return make_status_json(capture_state_name(st), s ? s->req.bridge : "",
                            sid, jpeg, 0, captured_total_.load());
  };
  // 报告任一非 IDLE 会话，否则报告最近一次
  if (north_ && north_->state.load() != CaptureState::IDLE) return snapshot(north_);
  if (south_ && south_->state.load() != CaptureState::IDLE) return snapshot(south_);
  return snapshot(north_);
}

std::string CaptureEngine::handle_health() const {
  bool healthy = !resource_fatal_.load();
  int active = 0;
  if (north_ && north_->state.load() != CaptureState::IDLE) active++;
  if (south_ && south_->state.load() != CaptureState::IDLE) active++;
  int64_t uptime = (wall_ms() - start_ms_.load()) / 1000;
  return make_health_json(healthy, uptime, active, captured_total_.load(),
                          cfg_.bmodel_path);
}

std::string CaptureEngine::handle_cancel(const std::string& session_id) {
  for (auto* s : {north_.get(), south_.get()}) {
    if (!s) continue;
    if (s->req.session_id == session_id && s->state.load() != CaptureState::IDLE) {
      s->cancel.store(true);
      return make_arm_response(true, session_id, "CANCELLED");
    }
  }
  return make_arm_response(false, session_id, "IDLE", "no active session");
}

void CaptureEngine::stop() {
  stop_.store(true);
  if (north_) { north_->cancel.store(true); if (north_->worker.joinable()) north_->worker.join(); north_->state.store(CaptureState::IDLE); }
  if (south_) { south_->cancel.store(true); if (south_->worker.joinable()) south_->worker.join(); south_->state.store(CaptureState::IDLE); }
}

bool CaptureEngine::open_source(SophonVideoSource& src, const BridgeCaptureConfig& bc,
                                std::string& err) {
  const char* env_val = std::getenv(bc.url_env.c_str());
  if (!env_val || !env_val[0]) {
    err = "环境变量 " + bc.url_env + " 未设置";
    return false;
  }
  std::string url = env_val;
  RtspSourceOptions ro;
  ro.transport = "tcp";
  ro.stimeout_us = 5000000;
  ro.max_reconnect_attempts = 2;  // 抓拍会话有限重连（生产 A/B 是无限，抓拍失败可接受）
  ro.initial_backoff_ms = 1000;
  ro.max_backoff_ms = 5000;
  if (!src.open_rtsp(url, cfg_.device, bc.extra_frame_buffer_num, "h264_bm", ro, err)) {
    if (src.resource_fatal()) resource_fatal_.store(true);
    return false;
  }
  std::fprintf(stdout, "信息 | capture | %s 视频打开 %dx%d\n",
               redact_url_credentials(url).c_str(), src.width(), src.height());
  std::fflush(stdout);
  return true;
}

void CaptureEngine::close_source_safe(SophonVideoSource& src, int drain_ms) {
  // 严格 VPU 生命周期（见 goal-objective 十六）：
  //   停止产生新帧(request_stop) -> 等待在途帧归零 -> 关闭解码器
  src.request_stop();
  if (!src.wait_avframes_drained(drain_ms)) {
    std::fprintf(stderr, "警告 | capture | 在途帧未归零，强制关闭可能泄漏 VPU 显存\n");
  }
  src.close();
}

void CaptureEngine::run_session(Session& s, const CaptureArmRequest& req) {
  const BridgeCaptureConfig* bc = bridge_cfg(req.bridge);
  if (!bc) { s.state.store(CaptureState::FAILED); return; }

  SophonVideoSource src;
  std::string err;
  // OPENING_STREAM
  if (!open_source(src, *bc, err)) {
    s.last_result.reason = err;
    s.state.store(src.resource_fatal() ? CaptureState::RESOURCE_FATAL : CaptureState::FAILED);
    if (src.resource_fatal()) resource_fatal_.store(true);
    return;
  }
  if (s.cancel.load()) { close_source_safe(src, bc->rtsp_drain_timeout_ms); s.state.store(CaptureState::CANCELLED); return; }

  // BMCV 按实际分辨率 init
  BmcvProcessor bmcv;
  if (!bmcv.init(detector_->handle(), src.width(), src.height(), err)) {
    s.last_result.reason = "BMCV init: " + err;
    close_source_safe(src, bc->rtsp_drain_timeout_ms);
    s.state.store(CaptureState::FAILED);
    return;
  }

  // SEARCHING：按 inference_fps 推理
  s.state.store(CaptureState::SEARCHING);
  const int input_size = 640;
  const int num_boxes = detector_->output_shape().size() >= 2 ? detector_->output_shape()[1] : 25200;
  const int num_vals = detector_->output_shape().size() >= 3 ? detector_->output_shape()[2] : 6;
  const int64_t infer_interval_ms = 1000 / bc->inference_fps;
  const int64_t deadline_ms = wall_ms() + (int64_t)req.timeout_sec * 1000;

  std::vector<float> input, output;
  std::vector<Detection> dets;
  bool bmcv_ready = bmcv.ready();

  // 最佳帧候选累积
  struct BestCand {
    float score = -1.0f;
    float bx1, by1, bx2, by2;
    int64_t ts_ms = 0;
  };
  BestCand best;
  bool in_candidate_window = false;
  int64_t candidate_start_ms = 0;
  bool has_prev = false;
  float prev_cx = 0, prev_cy = 0;
  int64_t last_infer_ms = 0;
  std::string berr;

  while (!stop_.load() && !s.cancel.load()) {
    if (src.resource_fatal()) { resource_fatal_.store(true); s.state.store(CaptureState::RESOURCE_FATAL); break; }
    if (wall_ms() > deadline_ms) { s.state.store(CaptureState::TIMEOUT); break; }

    // 推理频率调度
    int64_t t = wall_ms();
    if (last_infer_ms != 0 && (t - last_infer_ms) < infer_interval_ms) {
      // 简单睡眠到下次推理
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    VideoFrame vf;
    if (!src.read(vf, err)) {
      if (err == "停止") break;
      if (err == "RTSP_RECONNECT") {
        // 抓拍会话：有限重连已耗尽则失败
        if (!src.reconnect_rtsp(err)) {
          s.last_result.reason = "RTSP 重连失败: " + err;
          s.state.store(CaptureState::FAILED);
          break;
        }
        continue;
      }
      s.last_result.reason = "read: " + err;
      s.state.store(CaptureState::FAILED);
      break;
    }
    last_infer_ms = t;
    if (!vf.frame) { continue; }

    // 推理
    bool pre_ok = false;
    if (bmcv_ready) {
      input.assign(static_cast<size_t>(detector_->input_element_count()), 0.0f);
      pre_ok = bmcv.preprocess(vf.frame, input.data(), berr);
    }
    if (!pre_ok) { vf.release(); continue; }

    if (!detector_->infer(input, output)) { vf.release(); continue; }
    dets.clear();
    postprocess_yolov7(output.data(), num_boxes, num_vals, src.width(), src.height(),
                       input_size, bc->conf, bc->iou, dets);

    // 选最佳目标：valid_roi 内 + 非禁区 + 面积最大
    Detection* best_det = nullptr;
    float best_area = 0;
    for (auto& d : dets) {
      float cx = (d.x1 + d.x2) * 0.5f;
      float cy = (d.y1 + d.y2) * 0.5f;
      float nx = cx / src.width();
      float ny = cy / src.height();
      if (!bc->roi.point_in_valid(nx, ny)) continue;
      if (bc->roi.point_in_forbidden(nx, ny)) continue;
      float area = (d.x2 - d.x1) * (d.y2 - d.y1);
      if (area > best_area) { best_area = area; best_det = &d; }
    }

    if (best_det) {
      float cx = (best_det->x1 + best_det->x2) * 0.5f;
      float cy = (best_det->y1 + best_det->y2) * 0.5f;
      float nx = cx / src.width();
      float ny = cy / src.height();
      // 进入 capture_roi -> 开始候选窗口
      if (bc->roi.point_in_capture(nx, ny)) {
        if (!in_candidate_window) {
          in_candidate_window = true;
          candidate_start_ms = wall_ms();
          s.state.store(CaptureState::CANDIDATE);
        }
        CandidateScore sc = score_candidate(
            best_det->x1, best_det->y1, best_det->x2, best_det->y2,
            best_det->score, src.width(), src.height(), bc->roi,
            prev_cx, prev_cy, has_prev);
        if (sc.score > best.score) {
          best.score = sc.score;
          best.bx1 = best_det->x1; best.by1 = best_det->y1;
          best.bx2 = best_det->x2; best.by2 = best_det->y2;
          best.ts_ms = vf.capture_time_ms ? vf.capture_time_ms : wall_ms();
        }
        has_prev = true;
        prev_cx = cx; prev_cy = cy;

        // 候选窗口结束 -> 选最佳帧抓拍
        if (wall_ms() - candidate_start_ms >= bc->candidate_window_ms) {
          // CAPTURED：crop 原始帧 + JPEG
          s.state.store(CaptureState::CAPTURED);
          PaddedBox pb = pad_and_clamp_box(best.bx1, best.by1, best.bx2, best.by2,
                                           bc->bbox_padding, src.width(), src.height());
          // BMCV crop NV12 -> RGB（设备侧完成 crop+CSC）
          Image rgb_crop;
          bool crop_ok = false;
          if (bmcv_ready) {
            crop_ok = bmcv.crop_to_rgb(vf.frame, static_cast<int>(pb.x1), static_cast<int>(pb.y1),
                                       static_cast<int>(pb.x2 - pb.x1 + 1),
                                       static_cast<int>(pb.y2 - pb.y1 + 1), rgb_crop, berr);
          }
          vf.release();  // 立即释放 VPU 帧
          if (crop_ok && rgb_crop.valid()) {
            // 原子写：先 .tmp 再 rename
            std::string fname = build_capture_filename(req.bridge, req.mmsi, best.ts_ms, req.session_id);
            std::string ready_dir = cfg_.capture_dir + "/ready";
            std::string pending_dir = cfg_.capture_dir + "/pending";
            // 确保 ready 存在（daemon/部署负责；此处兜底）
            std::string tmp = pending_dir + "/." + fname + ".tmp";
            std::string final = ready_dir + "/" + fname;
            if (encode_jpeg(tmp, rgb_crop, bc->jpeg_quality)) {
              if (std::rename(tmp.c_str(), final.c_str()) == 0) {
                s.last_jpeg_path = final;
                captured_total_.fetch_add(1);
                s.last_result.state = CaptureState::CAPTURED;
                s.last_result.jpeg_path = final;
                s.last_result.captured_ts_ms = best.ts_ms;
                s.last_result.best_score = best.score;
              } else {
                s.last_result.reason = "rename 失败";
                s.state.store(CaptureState::FAILED);
              }
            } else {
              s.last_result.reason = "JPEG 编码失败";
              s.state.store(CaptureState::FAILED);
            }
          } else {
            s.last_result.reason = "BMCV crop 失败: " + berr;
            s.state.store(CaptureState::FAILED);
          }
          break;  // 抓拍完成，退出推理循环
        }
      }
      vf.release();
    } else {
      vf.release();
      // 无目标：若在候选窗口内但目标消失，结束窗口选当前最佳
      if (in_candidate_window && best.score >= 0 && wall_ms() - candidate_start_ms >= bc->candidate_window_ms) {
        // 回到搜索，等待目标重现（不强制抓空帧）
      }
    }
  }

  // CLOSING -> IDLE：严格关闭
  if (s.state.load() != CaptureState::CAPTURED &&
      s.state.load() != CaptureState::RESOURCE_FATAL &&
      !capture_state_terminal(s.state.load())) {
    // 未到终态则视为失败/超时
    s.state.store(s.cancel.load() ? CaptureState::CANCELLED : CaptureState::FAILED);
  }
  close_source_safe(src, bc->rtsp_drain_timeout_ms);
  // 回到 IDLE（保留 last_result 供 status 查询，state 显式置 IDLE）
  s.state.store(CaptureState::IDLE);
}

}  // namespace hzw
