// -*- coding: utf-8 -*-
// =============================================================================
// CaptureEngine：事件驱动的视频抓拍引擎（复用 hzw_inf，不复制源码）。
//
// 职责（见 goal-objective 九~十六）：
//   - 启动时加载 1×BmrtDetector（north/south 共用），进入 IDLE。
//   - 收到 arm -> 异步开对应视频 -> 3~5FPS 推理 -> 找船 -> 最佳帧 -> 原始帧 crop -> JPEG。
//   - 同一 bridge 同时只允许一个 active session；南北最多两个。
//   - 严格 VPU 生命周期：停止产生新帧 -> drain -> inflight==0 -> 关闭解码器。
//   - IDLE 时不开视频、不占 VPU、不推理。
//
// 本类依赖 BM1684 硬件；纯逻辑测试覆盖 capture_core/protocol，硬件测试靠板端 CLI。
// =============================================================================
#ifndef HZW_CAPTURE_CAPTURE_ENGINE_H
#define HZW_CAPTURE_CAPTURE_ENGINE_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "capture/capture_core.h"
#include "inference/bmrt_detector.h"
#include "video/bmcv_processor.h"
#include "video/video_source.h"

namespace hzw {

struct BridgeCaptureConfig {
  std::string bridge;        // "north" | "south"
  std::string url_env;       // RTSP URL 环境变量名（真实 URL 不入命令行/日志）
  std::string test_file;     // 测试用本地文件路径（非空时优先于 RTSP，板端验证用）
  CaptureRoi roi;
  int inference_fps = 5;          // 单路 active
  int inference_fps_shared = 3;   // 南北同时 active 时
  int candidate_window_ms = 1500; // 最佳帧候选窗口
  int session_timeout_sec = 180;
  int rtsp_drain_timeout_ms = 5000;
  int extra_frame_buffer_num = 20;
  int jpeg_quality = 92;
  float bbox_padding = 0.15f;
  float conf = 0.1f;
  float iou = 0.1f;
};

struct CaptureEngineConfig {
  int device = 0;
  std::string bmodel_path;
  std::string capture_dir;   // 如 /data/hangzhouwan/bridge/captures
  BridgeCaptureConfig north;
  BridgeCaptureConfig south;
  std::string socket_path = "/run/hangzhouwan/bridge-capture.sock";
  int disk_free_threshold_mb = 500;  // 可用磁盘低于此值时拒绝新抓拍（保护生产）
};

class CaptureEngine {
 public:
  CaptureEngine();
  ~CaptureEngine();

  CaptureEngine(const CaptureEngine&) = delete;
  CaptureEngine& operator=(const CaptureEngine&) = delete;

  // 加载 bmodel，准备 BMCV。失败返回 false（err 给出原因）。
  bool init(const CaptureEngineConfig& cfg, std::string& err);

  // 处理 arm 请求（异步启动会话，立即返回）。线程安全。
  // 返回立即响应 JSON（accepted + state）。
  std::string handle_arm(const CaptureArmRequest& req);

  // 查询状态 JSON（最近会话 + 全局计数）。
  std::string handle_status() const;
  std::string handle_health() const;

  // 取消指定 bridge 的会话。
  std::string handle_cancel(const std::string& session_id);

  // 是否资源致命（VPU/BMRuntime 耗尽）。
  bool resource_fatal() const { return resource_fatal_.load(); }

  // 停止所有会话并等待 worker 退出。
  void stop();

 private:
  struct Session;
  const BridgeCaptureConfig* bridge_cfg(const std::string& bridge) const;
  void run_session(Session& s, const CaptureArmRequest& req);
  bool open_source(SophonVideoSource& src, const BridgeCaptureConfig& bc,
                   std::string& err);
  void close_source_safe(SophonVideoSource& src, int drain_ms);

  CaptureEngineConfig cfg_;
  std::unique_ptr<BmrtDetector> detector_;
  BmcvProcessor bmcv_;
  std::atomic<bool> resource_fatal_{false};
  std::atomic<int64_t> captured_total_{0};
  std::atomic<int64_t> start_ms_{0};

  // 每个 bridge 一个 session 槽（互斥保护状态切换）
  struct Session {
    std::mutex mtx;
    std::thread worker;
    std::atomic<CaptureState> state{CaptureState::IDLE};
    std::atomic<bool> cancel{false};
    CaptureArmRequest req;
    CaptureResult last_result;
    std::string last_jpeg_path;
  };
  std::unique_ptr<Session> north_;
  std::unique_ptr<Session> south_;
  std::atomic<bool> stop_{false};
};

}  // namespace hzw

#endif  // HZW_CAPTURE_CAPTURE_ENGINE_H
