// -*- coding: utf-8 -*-
// Bridge Capture 纯逻辑核心实现（无硬件依赖，可单元测试）。
#include "capture/capture_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace hzw {

const char* capture_state_name(CaptureState s) {
  switch (s) {
    case CaptureState::IDLE:           return "IDLE";
    case CaptureState::OPENING_STREAM: return "OPENING_STREAM";
    case CaptureState::SEARCHING:      return "SEARCHING";
    case CaptureState::CANDIDATE:      return "CANDIDATE";
    case CaptureState::CAPTURED:       return "CAPTURED";
    case CaptureState::CLOSING:        return "CLOSING";
    case CaptureState::FAILED:         return "FAILED";
    case CaptureState::TIMEOUT:        return "TIMEOUT";
    case CaptureState::CANCELLED:      return "CANCELLED";
    case CaptureState::RESOURCE_FATAL: return "RESOURCE_FATAL";
  }
  return "UNKNOWN";
}

bool capture_state_terminal(CaptureState s) {
  return s == CaptureState::CAPTURED || s == CaptureState::FAILED ||
         s == CaptureState::TIMEOUT || s == CaptureState::CANCELLED ||
         s == CaptureState::RESOURCE_FATAL;
}

// ---- CaptureRoi ----
static bool in_rect(double nx, double ny, double x1, double y1, double x2, double y2) {
  return nx >= x1 && nx <= x2 && ny >= y1 && ny <= y2;
}

bool CaptureRoi::point_in_valid(double nx, double ny) const {
  return in_rect(nx, ny, valid_x1, valid_y1, valid_x2, valid_y2);
}

bool CaptureRoi::point_in_capture(double nx, double ny) const {
  return in_rect(nx, ny, cap_x1, cap_y1, cap_x2, cap_y2);
}

bool CaptureRoi::point_in_forbidden(double nx, double ny) const {
  for (const auto& f : forbidden) {
    if (in_rect(nx, ny, f[0], f[1], f[2], f[3])) return true;
  }
  return false;
}

// ---- bbox padding ----
PaddedBox pad_and_clamp_box(float x1, float y1, float x2, float y2,
                            float padding_frac, int img_w, int img_h) {
  float w = x2 - x1;
  float h = y2 - y1;
  float pad_w = w * padding_frac;
  float pad_h = h * padding_frac;
  float px1 = x1 - pad_w;
  float py1 = y1 - pad_h;
  float px2 = x2 + pad_w;
  float py2 = y2 + pad_h;
  // clamp 到图像边界
  px1 = std::max(0.0f, px1);
  py1 = std::max(0.0f, py1);
  px2 = std::min(static_cast<float>(img_w) - 1.0f, px2);
  py2 = std::min(static_cast<float>(img_h) - 1.0f, py2);
  if (px2 < px1) px2 = px1;
  if (py2 < py1) py2 = py1;
  return {px1, py1, px2, py2};
}

// ---- 候选评分 ----
static float iou_xyxy_f(float ax1, float ay1, float ax2, float ay2,
                        float bx1, float by1, float bx2, float by2) {
  float ix1 = std::max(ax1, bx1), iy1 = std::max(ay1, by1);
  float ix2 = std::min(ax2, bx2), iy2 = std::min(ay2, by2);
  float iw = std::max(0.0f, ix2 - ix1);
  float ih = std::max(0.0f, iy2 - iy1);
  float inter = iw * ih;
  float a = std::max(0.0f, (ax2 - ax1)) * std::max(0.0f, (ay2 - ay1));
  float b = std::max(0.0f, (bx2 - bx1)) * std::max(0.0f, (by2 - by1));
  float uni = a + b - inter;
  return uni > 0.0f ? inter / uni : 0.0f;
}

CandidateScore score_candidate(
    float bx1, float by1, float bx2, float by2, float conf,
    int img_w, int img_h,
    const CaptureRoi& roi,
    float prev_cx, float prev_cy, bool has_prev) {
  CandidateScore cs;
  cs.conf = conf;
  float cx = (bx1 + bx2) * 0.5f;
  float cy = (by1 + by2) * 0.5f;
  float nx = cx / static_cast<float>(img_w);
  float ny = cy / static_cast<float>(img_h);

  // 中心到 capture_roi 中心的距离（归一化，越小越好）
  float rcx = (roi.cap_x1 + roi.cap_x2) * 0.5f;
  float rcy = (roi.cap_y1 + roi.cap_y2) * 0.5f;
  cs.center_dist = std::sqrt((nx - rcx) * (nx - rcx) + (ny - rcy) * (ny - rcy));

  // 面积归一化
  float bw = std::max(0.0f, bx2 - bx1);
  float bh = std::max(0.0f, by2 - by1);
  cs.area_norm = (bw * bh) / static_cast<float>(img_w * img_h);

  // 贴边惩罚：距离四条边的最小归一化距离
  float edge_min = std::min({nx, ny, 1.0f - nx, 1.0f - ny});
  cs.edge_penalty = edge_min < 0.02f ? (0.02f - edge_min) / 0.02f : 0.0f;

  // 连续性：与上一帧中心的距离（归一化）
  if (has_prev) {
    float dx = (cx - prev_cx) / static_cast<float>(img_w);
    float dy = (cy - prev_cy) / static_cast<float>(img_h);
    float d = std::sqrt(dx * dx + dy * dy);
    // 距离小 -> 连续性高
    cs.continuity = std::max(0.0f, 1.0f - d * 4.0f);
  } else {
    cs.continuity = 0.5f;  // 首帧中性
  }

  // 加权综合（各项先归一到 [0,1]）
  float center_score = std::max(0.0f, 1.0f - cs.center_dist * 2.0f);
  float area_score = std::min(1.0f, cs.area_norm * 8.0f);  // 12.5% 图像即满分
  float conf_score = std::min(1.0f, conf);
  float cont = cs.continuity;

  // 权重：中心 0.35 / 面积 0.20 / 置信 0.25 / 连续 0.20，减贴边惩罚
  cs.score = 0.35f * center_score + 0.20f * area_score +
             0.25f * conf_score + 0.20f * cont;
  cs.score *= (1.0f - 0.5f * cs.edge_penalty);
  if (cs.score < 0.0f) cs.score = 0.0f;
  if (cs.score > 1.0f) cs.score = 1.0f;
  return cs;
}

// ---- 文件命名 ----
// bridge_mmsi_YYYYmmddHHMMSS_sessionid8.jpg
std::string build_capture_filename(const std::string& bridge, const std::string& mmsi,
                                   int64_t capture_ts_ms, const std::string& session_id) {
  std::time_t t = capture_ts_ms / 1000;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  // session_id 取前 8 字符
  std::string sid = session_id.size() > 8 ? session_id.substr(0, 8) : session_id;
  if (sid.empty()) sid = "nosession";
  return bridge + "_" + mmsi + "_" + buf + "_" + sid + ".jpg";
}

}  // namespace hzw
