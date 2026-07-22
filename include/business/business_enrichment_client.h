// -*- coding: utf-8 -*-
// =============================================================================
// BusinessEnrichmentClient：C++ 侧业务增强客户端（阶段4.4）。
//
// 通过 Unix Domain Socket 与 Python 业务 Sidecar 通信：
//   - 每帧批量发送检测框坐标
//   - 接收坐标预测 + AIS 匹配结果
//   - 严格超时（默认 30ms），超时降级为仅检测
//   - 容量1最新请求策略：Sidecar 忙时丢弃旧请求
//   - Sidecar 不可用时不阻塞视频热路径
//
// 降级状态：
//   FULL          - 坐标 + AIS 匹配成功
//   COORD_ONLY    - 坐标成功，AIS 不可用
//   DETECTION_ONLY - Sidecar 超时或不可用
// =============================================================================
#ifndef HZW_BUSINESS_BUSINESS_ENRICHMENT_CLIENT_H
#define HZW_BUSINESS_BUSINESS_ENRICHMENT_CLIENT_H

#include <cstdint>
#include <string>
#include <vector>

namespace hzw {

struct BusinessResult {
  int detection_id = 0;
  double longitude = 0.0;
  double latitude = 0.0;
  bool coordinate_valid = false;
  bool ais_matched = false;
  std::string mmsi;
  std::string ship_name;
  double speed = 0.0;
  double course = 0.0;
  double ais_distance_km = 0.0;
  int ais_age_seconds = 0;
  // 完整证据字段（AIS 证据 + JSONL）
  int64_t ais_age_ms = 0;
  double match_score = 0.0;
  std::string reject_reason;
  bool extrapolated = false;
  double ais_lon = 0.0;        // AIS 原始经度
  double ais_lat = 0.0;        // AIS 原始纬度
  double ais_lon_aligned = 0.0; // AIS 时间对齐后经度
  double ais_lat_aligned = 0.0; // AIS 时间对齐后纬度
  float score = 0.0f;          // 检测置信度
  float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;  // 检测框
};

enum class EnrichmentState {
  FULL,
  COORD_ONLY,
  DETECTION_ONLY,
};

struct DetectionBox {
  int detection_id = 0;
  float score = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
  float x2 = 0.0f;
  float y2 = 0.0f;
};

class BusinessEnrichmentClient {
 public:
  BusinessEnrichmentClient();
  ~BusinessEnrichmentClient();

  // 连接到 Sidecar socket。失败返回 false（可重试）。
  bool connect(const std::string& socket_path);

  // 批量请求：发送检测框，接收坐标+匹配结果。
  // 超时（timeout_ms）或连接失败时返回 false 并设置 state=DETECTION_ONLY。
  bool enrich(const std::string& stream_id, int64_t frame_sequence,
              int image_width, int image_height,
              const std::vector<DetectionBox>& detections,
              std::vector<BusinessResult>& results,
              int timeout_ms = 30);

  EnrichmentState state() const { return state_; }
  int degrade_count() const { return degrade_count_; }
  int recover_count() const { return recover_count_; }

 private:
  int fd_ = -1;
  std::string socket_path_;
  EnrichmentState state_ = EnrichmentState::DETECTION_ONLY;
  int degrade_count_ = 0;
  int recover_count_ = 0;
};

}  // namespace hzw

#endif  // HZW_BUSINESS_BUSINESS_ENRICHMENT_CLIENT_H
