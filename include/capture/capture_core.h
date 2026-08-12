// -*- coding: utf-8 -*-
// =============================================================================
// Bridge Capture 纯逻辑核心：状态机 / ROI / 候选评分 / 协议模型。
//
// 设计目标（见 goal-objective P2/P3）：
//   - small / explicit / testable / replaceable / observable
//   - 硬件无关逻辑（状态机、ROI 判定、bbox padding、候选评分、文件命名、协议帧）
//     全部集中于此，可独立单元测试（无需 BM1684/VPU/FFmpeg）。
//   - 硬件相关（视频源/BmrtDetector/BMCV/JPEG）在 capture_engine 中按状态机驱动，
//     复用 hzw_inf 现有组件，不复制源码。
//
// 抓拍任务生命周期（每个 bridge 同时只允许一个 active session）：
//   IDLE -> OPENING_STREAM -> SEARCHING -> CANDIDATE -> CAPTURED -> CLOSING -> IDLE
//   失败终态：FAILED / TIMEOUT / CANCELLED / RESOURCE_FATAL
// =============================================================================
#ifndef HZW_CAPTURE_CAPTURE_CORE_H
#define HZW_CAPTURE_CAPTURE_CORE_H

#include <cstdint>
#include <string>
#include <array>
#include <vector>

namespace hzw {

// ---- 抓拍会话状态 ----
enum class CaptureState {
  IDLE = 0,
  OPENING_STREAM,
  SEARCHING,
  CANDIDATE,
  CAPTURED,
  CLOSING,
  // 终态/失败
  FAILED,
  TIMEOUT,
  CANCELLED,
  RESOURCE_FATAL,
};

// 状态名（日志/协议用）。
const char* capture_state_name(CaptureState s);

// 是否终态（非 IDLE 的工作完成或失败终态，需回到 IDLE）。
bool capture_state_terminal(CaptureState s);

// ---- 单座桥的抓拍 ROI 配置（归一化 0~1，相对原图）----
struct CaptureRoi {
  // valid_roi：船舶才被视为有效目标的区域（中心点落在其中）。
  double valid_x1 = 0.0, valid_y1 = 0.0, valid_x2 = 1.0, valid_y2 = 1.0;
  // capture_roi：目标进入此区域才开始累积最佳帧候选。
  double cap_x1 = 0.0, cap_y1 = 0.0, cap_x2 = 1.0, cap_y2 = 1.0;
  // forbidden_regions：中心落入则丢弃（已有 DetectionRegionFilter 亦可用）。
  std::vector<std::array<double,4>> forbidden;  // {x1,y1,x2,y2} 归一化

  bool point_in_valid(double nx, double ny) const;
  bool point_in_capture(double nx, double ny) const;
  bool point_in_forbidden(double nx, double ny) const;
};

// ---- bbox padding / clamp（原图坐标）----
// padding_frac 0.1~0.2；clamp 到 [0,w)/[0,h)。返回 padded xyxy。
struct PaddedBox { float x1, y1, x2, y2; };
PaddedBox pad_and_clamp_box(float x1, float y1, float x2, float y2,
                            float padding_frac, int img_w, int img_h);

// ---- 候选帧评分（综合：中心距离 / 面积 / 置信度 / 边缘裁切惩罚 / 连续性）----
struct CandidateScore {
  float score = 0.0f;
  float center_dist = 0.0f;   // 归一化中心到 capture_roi 中心的距离（越小越好）
  float area_norm = 0.0f;     // bbox 面积 / 图像面积
  float conf = 0.0f;          // 检测置信度
  float edge_penalty = 0.0f;  // 贴边惩罚
  float continuity = 0.0f;    // 与上一帧 IoU（越大越好）
};

// 计算单个候选的评分。返回 [0,1] 归一化总分。
CandidateScore score_candidate(
    float bx1, float by1, float bx2, float by2, float conf,
    int img_w, int img_h,
    const CaptureRoi& roi,
    float prev_cx, float prev_cy, bool has_prev);

// ---- 抓拍会话（状态机）----
struct CaptureArmRequest {
  std::string session_id;
  std::string bridge;          // "north" | "south"
  std::string mmsi;
  std::string direction;       // upstream | downstream
  int64_t trigger_ts_ms = 0;
  double distance_to_gate_m = 0.0;
  double eta_sec = 0.0;
  int timeout_sec = 180;
};

struct CaptureResult {
  std::string session_id;
  std::string bridge;
  CaptureState state = CaptureState::IDLE;
  std::string jpeg_path;       // 成功时落盘路径
  std::string reason;          // 失败原因
  int64_t captured_ts_ms = 0;
  float best_score = 0.0f;
};

// 文件命名：bridge_mmsi_YYYYmmddHHMMSS_session8.jpg
std::string build_capture_filename(const std::string& bridge,
                                   const std::string& mmsi,
                                   int64_t capture_ts_ms,
                                   const std::string& session_id);

// UDS 协议（与 bridge_control.py 同 4B 长度前缀 JSON）：
//   入：{"cmd":"arm","session_id":...,"bridge":"north","mmsi":...,...}
//       {"cmd":"status"},{"cmd":"cancel","session_id":...},{"cmd":"health"}
//   出：{"accepted":true,"session_id":...,"state":"QUEUED"} 等
// 协议帧编解码见 capture_protocol.h（纯字符串/JSON，无硬件依赖）。

}  // namespace hzw

#endif  // HZW_CAPTURE_CAPTURE_CORE_H
