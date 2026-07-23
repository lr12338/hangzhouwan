// -*- coding: utf-8 -*-
// =============================================================================
// PipelineMetrics：管线运行指标统计（线程安全）。
//
// 阶段4.4 分段指标：RTSP packet/读取成功/解码/调度输出/入队/丢帧/预处理/推理/
// 后处理/禁区/绘制/编码送帧/编码packet/RTMP写入成功/失败/重连/最近输入年龄。
// 耗时分段：RTSP读取/硬解/BMCV VPP/Host归一化/推理/后处理/禁区/绘制/编码/RTMP写入/端到端。
// 每 N 秒输出一次汇总（不逐帧刷 INFO）。
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
  // ===== 计数器（原子，高频更新）=====
  std::atomic<int64_t> read_frames{0};           // RTSP packet 数（av_read_frame 调用次数）
  std::atomic<int64_t> read_success{0};          // av_read_frame 成功次数
  std::atomic<int64_t> decoded_frames{0};        // 解码帧数
  std::atomic<int64_t> scheduled_output{0};      // 调度允许输出帧数（进入解码队列前）
  std::atomic<int64_t> queued_frames{0};         // 进入处理队列帧数
  std::atomic<int64_t> decode_queue_dropped{0};  // 解码队列丢帧数
  std::atomic<int64_t> dropped_frames{0};        // 处理队列丢帧数
  std::atomic<int64_t> preprocess_count{0};      // 预处理完成数
  std::atomic<int64_t> inference_count{0};       // 推理次数
  std::atomic<int64_t> empty_detection{0};       // 空检测次数
  std::atomic<int64_t> encode_sent{0};           // 编码送帧数
  std::atomic<int64_t> encode_packets{0};        // 编码输出 packet 数
  std::atomic<int64_t> output_frames{0};         // RTMP/文件写入成功数
  std::atomic<int64_t> rtmp_write_fail{0};       // RTMP 写入失败数
  std::atomic<int64_t> rtsp_reconnects{0};       // RTSP 重连次数
  std::atomic<int64_t> rtmp_reconnects{0};       // RTMP 重连次数
  std::atomic<int64_t> last_read_ms{0};          // 最近一次成功读取的墙钟时间（ms）
  std::string stream_id;                           // 流标识（A/B），用于日志前缀

  // ===== 计时采样（毫秒）=====
  void record_read_ms(double v) { push(read_ms_, v); }
  void record_decode_ms(double v) { push(decode_ms_, v); }
  void record_vpp_ms(double v) { push(vpp_ms_, v); }
  void record_normalize_ms(double v) { push(normalize_ms_, v); }
  void record_pre_ms(double v) { push(pre_ms_, v); }  // 兼容：总预处理（VPP+归一化）
  void record_infer_ms(double v) { push(infer_ms_, v); }
  void record_post_ms(double v) { push(post_ms_, v); }
  void record_region_ms(double v) { push(region_ms_, v); }
  void record_draw_ms(double v) { push(draw_ms_, v); }
  void record_encode_ms(double v) { push(encode_ms_, v); }
  void record_rtmp_ms(double v) { push(rtmp_ms_, v); }
  void record_e2e_ms(double v);

  // 设置当前队列长度（供汇总展示）。
  void set_queue_length(int len) { queue_len_.store(len); }

  // 设置 jitter buffer 长度（供汇总展示）。
  void set_jitter_length(int len) { jitter_len_.store(len); }

  // 健康接口用的 P95 采样（毫秒）。
  double e2e_p95() const { std::lock_guard<std::mutex> lk(m_); return p95(e2e_ms_); }
  double infer_p95() const { std::lock_guard<std::mutex> lk(m_); return p95(infer_ms_); }
  int queue_length() const { return queue_len_.load(); }

  // 输出一次汇总日志（中文）。
  std::string summary() const;

  // 重置（每轮稳定性测试前）。
  void reset();

 private:
  static void push(std::deque<double>& d, double v);
  static double mean(const std::deque<double>& d);
  static double p95(const std::deque<double>& d);

  mutable std::mutex m_;
  std::deque<double> read_ms_, decode_ms_, vpp_ms_, normalize_ms_, pre_ms_,
      infer_ms_, post_ms_, region_ms_, draw_ms_, encode_ms_, rtmp_ms_, e2e_ms_;
  std::atomic<int> queue_len_{0};
  std::atomic<int> jitter_len_{0};
};

}  // namespace hzw

#endif  // HZW_MONITORING_PIPELINE_METRICS_H
