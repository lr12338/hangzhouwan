// -*- coding: utf-8 -*-
// =============================================================================
// PipelineMetrics：管线运行指标统计（线程安全）。
//
// 记录：读取/解码/入队/丢帧/输出/推理/空检测计数，以及解码/预处理/推理/后处理/编码
// 耗时均值与 P95，端到端延迟（当前/最大/P95），队列长度。
// 每 10 秒输出一次汇总（不逐帧刷 INFO）。
// =============================================================================
#ifndef HZW_MONITORING_PIPELINE_METRICS_H
#define HZW_MONITORING_PIPELINE_METRICS_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace hzw {

class PipelineMetrics {
 public:
  // 计数器（原子，高频更新）。
  std::atomic<int64_t> read_frames{0};       // 读取 packet 数
  std::atomic<int64_t> decoded_frames{0};    // 解码帧数
  std::atomic<int64_t> queued_frames{0};     // 进入处理队列帧数
  std::atomic<int64_t> dropped_frames{0};    // 队列丢帧数
  std::atomic<int64_t> output_frames{0};     // 编码输出帧数
  std::atomic<int64_t> inference_count{0};   // 推理次数
  std::atomic<int64_t> empty_detection{0};   // 空检测次数

  // 计时采样（毫秒），保留最近窗口用于均值/P95。
  void record_decode_ms(double v) { push(decode_ms_, v); }
  void record_pre_ms(double v) { push(pre_ms_, v); }
  void record_infer_ms(double v) { push(infer_ms_, v); }
  void record_post_ms(double v) { push(post_ms_, v); }
  void record_encode_ms(double v) { push(encode_ms_, v); }
  void record_e2e_ms(double v);

  // 设置当前队列长度（供汇总展示）。
  void set_queue_length(int len) { queue_len_.store(len); }

  // 输出一次汇总日志（中文），并清空 P95 窗口外的旧采样。
  std::string summary() const;

  // 重置（每轮稳定性测试前）。
  void reset();

 private:
  static void push(std::deque<double>& d, double v);
  static double mean(const std::deque<double>& d);
  static double p95(const std::deque<double>& d);

  mutable std::mutex m_;
  std::deque<double> decode_ms_, pre_ms_, infer_ms_, post_ms_, encode_ms_, e2e_ms_;
  std::atomic<int> queue_len_{0};
};

}  // namespace hzw

#endif  // HZW_MONITORING_PIPELINE_METRICS_H
