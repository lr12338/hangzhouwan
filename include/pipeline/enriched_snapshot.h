// -*- coding: utf-8 -*-
// =============================================================================
// EnrichedDetection / EnrichedDetectionSnapshot / EnrichedSnapshotStore：
// 融合业务增强结果的检测快照（生产收口）。
//
// 处理顺序：检测 -> NMS -> 禁区过滤 -> Sidecar 批量业务增强 ->
//           EnrichedSnapshot 更新 -> 绘框 -> 编码推流。
//
// Sidecar 超时/不可用时：保留 Detection，enrichment_status=DETECTION_ONLY，
// 不阻断推流。
// =============================================================================
#ifndef HZW_PIPELINE_ENRICHED_SNAPSHOT_H
#define HZW_PIPELINE_ENRICHED_SNAPSHOT_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "business/business_enrichment_client.h"  // BusinessResult, EnrichmentState
#include "inference/yolov7_postprocess.h"          // Detection

namespace hzw {

// 单个检测 + 业务增强结果。
struct EnrichedDetection {
  // 基础检测
  float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
  float score = 0.0f;
  int detection_id = 0;
  // 坐标
  double longitude = 0.0;
  double latitude = 0.0;
  bool coordinate_valid = false;
  // AIS 匹配
  bool ais_matched = false;
  std::string mmsi;
  std::string ship_name;
  double speed = 0.0;
  double course = 0.0;
  double ais_distance_m = 0.0;
  int64_t ais_age_ms = 0;
  double match_score = 0.0;
  std::string reject_reason;
  bool extrapolated = false;
  double ais_lon = 0.0;
  double ais_lat = 0.0;
  double ais_lon_aligned = 0.0;
  double ais_lat_aligned = 0.0;
  // 增强状态
  EnrichmentState enrichment_status = EnrichmentState::DETECTION_ONLY;

  // 从 Detection 构造（仅检测，未增强）。
  explicit EnrichedDetection(const Detection& d)
      : x1(d.x1), y1(d.y1), x2(d.x2), y2(d.y2), score(d.score),
        enrichment_status(EnrichmentState::DETECTION_ONLY) {}
  EnrichedDetection() = default;

  // 从 Detection + BusinessResult 融合。
  EnrichedDetection(const Detection& d, const BusinessResult& r)
      : x1(d.x1), y1(d.y1), x2(d.x2), y2(d.y2), score(d.score),
        detection_id(r.detection_id),
        longitude(r.longitude), latitude(r.latitude),
        coordinate_valid(r.coordinate_valid),
        ais_matched(r.ais_matched),
        mmsi(r.mmsi), ship_name(r.ship_name),
        speed(r.speed), course(r.course),
        ais_distance_m(r.ais_distance_km * 1000.0),
        ais_age_ms(r.ais_age_ms),
        match_score(r.match_score),
        reject_reason(r.reject_reason),
        extrapolated(r.extrapolated),
        ais_lon(r.ais_lon), ais_lat(r.ais_lat),
        ais_lon_aligned(r.ais_lon_aligned), ais_lat_aligned(r.ais_lat_aligned),
        enrichment_status(EnrichmentState::DETECTION_ONLY) {
    if (ais_matched) {
      enrichment_status = EnrichmentState::FULL;
    } else if (coordinate_valid) {
      enrichment_status = EnrichmentState::COORD_ONLY;
    }
  }
};

// 一帧的融合快照。
struct EnrichedDetectionSnapshot {
  int64_t source_sequence = -1;
  int64_t source_pts = 0;
  int64_t generated_time_ms = 0;
  std::string coordinate_mode;
  std::vector<EnrichedDetection> detections;
  EnrichmentState enrichment_status = EnrichmentState::DETECTION_ONLY;

  bool valid() const { return source_sequence >= 0; }
  bool expired(int64_t now_ms, int64_t ttl_ms) const {
    if (!valid()) return true;
    return (now_ms - generated_time_ms) > ttl_ms;
  }
};

// 线程安全的融合快照容器。
class EnrichedSnapshotStore {
 public:
  void update(const EnrichedDetectionSnapshot& snap) {
    std::lock_guard<std::mutex> lk(m_);
    snap_ = snap;
  }
  EnrichedDetectionSnapshot get() const {
    std::lock_guard<std::mutex> lk(m_);
    return snap_;
  }
  void clear() {
    std::lock_guard<std::mutex> lk(m_);
    snap_ = EnrichedDetectionSnapshot{};
  }

 private:
  mutable std::mutex m_;
  EnrichedDetectionSnapshot snap_{};
};

// 将 enrichment_status 转为字符串（JSONL/日志用）。
inline const char* enrichment_status_str(EnrichmentState s) {
  switch (s) {
    case EnrichmentState::PENDING: return "PENDING";
    case EnrichmentState::FULL: return "FULL";
    case EnrichmentState::COORD_ONLY: return "COORD_ONLY";
    case EnrichmentState::DETECTION_ONLY: return "DETECTION_ONLY";
  }
  return "DETECTION_ONLY";
}

}  // namespace hzw

#endif  // HZW_PIPELINE_ENRICHED_SNAPSHOT_H
