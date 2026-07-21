// -*- coding: utf-8 -*-
// =============================================================================
// DetectionSnapshot：最近一次推理结果快照，用于低频推理 + 非推理帧复用。
//
// 语义（阶段4 不做目标跟踪，仅为显示策略）：
//   - 推理帧更新 snapshot（含来源帧 sequence/PTS 与生成时刻）。
//   - 非推理帧复用最近 snapshot 绘框；超过 result_ttl_ms 后视为过期，不再绘制。
//   - 不得将结果永久粘贴到后续帧。
// =============================================================================
#ifndef HZW_PIPELINE_DETECTION_SNAPSHOT_H
#define HZW_PIPELINE_DETECTION_SNAPSHOT_H

#include <cstdint>
#include <mutex>
#include <vector>
#include "inference/yolov7_postprocess.h"

namespace hzw {

struct DetectionSnapshot {
  int64_t source_sequence = -1;   // 来源推理帧序号；-1 表示无效
  int64_t source_pts = 0;
  int64_t generated_time_ms = 0;  // 生成时刻（墙钟 ms）
  std::vector<Detection> detections;

  bool valid() const { return source_sequence >= 0; }

  // now_ms 为当前墙钟 ms；ttl_ms 为结果有效期。
  bool expired(int64_t now_ms, int64_t ttl_ms) const {
    if (!valid()) return true;
    return (now_ms - generated_time_ms) > ttl_ms;
  }
};

// 线程安全的 snapshot 容器（处理线程读写，指标线程可能读）。
class SnapshotStore {
 public:
  void update(const DetectionSnapshot& snap) {
    std::lock_guard<std::mutex> lk(m_);
    snap_ = snap;
  }
  // 读取当前 snapshot 的拷贝。
  DetectionSnapshot get() const {
    std::lock_guard<std::mutex> lk(m_);
    return snap_;
  }

 private:
  mutable std::mutex m_;
  DetectionSnapshot snap_;
};

}  // namespace hzw

#endif  // HZW_PIPELINE_DETECTION_SNAPSHOT_H
