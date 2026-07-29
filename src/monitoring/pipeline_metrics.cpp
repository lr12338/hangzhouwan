// -*- coding: utf-8 -*-
#include "monitoring/pipeline_metrics.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <sstream>

namespace hzw {

namespace {
int64_t now_ms_internal() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

void PipelineMetrics::push(std::deque<double>& d, double v) {
  d.push_back(v);
  if (d.size() > 2000) d.pop_front();
}

double PipelineMetrics::mean(const std::deque<double>& d) {
  if (d.empty()) return 0.0;
  double s = 0;
  for (double v : d) s += v;
  return s / static_cast<double>(d.size());
}

double PipelineMetrics::p95(const std::deque<double>& d) {
  return percentile(d, 0.95);
}

double PipelineMetrics::percentile(const std::deque<double>& d,
                                   double fraction) {
  if (d.empty()) return 0.0;
  std::vector<double> v(d.begin(), d.end());
  std::sort(v.begin(), v.end());
  size_t idx = static_cast<size_t>(v.size() * fraction);
  if (idx >= v.size()) idx = v.size() - 1;
  return v[idx];
}

void PipelineMetrics::record_e2e_ms(double v) {
  std::lock_guard<std::mutex> lk(m_);
  push(e2e_ms_, v);
}

double PipelineMetrics::interarrival_p99() const {
  std::lock_guard<std::mutex> lk(m_);
  return percentile(interarrival_ms_, 0.99);
}

std::string PipelineMetrics::summary() const {
  std::lock_guard<std::mutex> lk(m_);
  // 最近输入年龄
  int64_t age = 0;
  int64_t lr = last_read_ms.load();
  if (lr > 0) age = now_ms_internal() - lr;

  std::ostringstream os;
  os << "信息 | 视频管线 | " << (stream_id.empty() ? "" : "[" + stream_id + "] ")
     << "pkt=" << read_frames.load()
     << " 读取=" << read_success.load()
     << " 解码=" << decoded_frames.load()
     << " 调度输出=" << scheduled_output.load()
     << " 入队=" << queued_frames.load()
     << " 推理=" << inference_count.load()
     << " 空检=" << empty_detection.load()
     << " 编码送帧=" << encode_sent.load()
     << " 输出=" << output_frames.load()
     << " RTMP失败=" << rtmp_write_fail.load()
     << " RTSP重连=" << rtsp_reconnects.load()
     << " RTMP重连=" << rtmp_reconnects.load()
     << " RTSP尝试/失败=" << rtsp_reconnect_attempts.load()
     << "/" << rtsp_reconnect_failures.load()
     << " RTMP尝试/失败=" << rtmp_reconnect_attempts.load()
     << "/" << rtmp_reconnect_failures.load()
     << " 最近输入=" << age << "ms"
     << "\n信息 | 丢帧 | "
     << "解码队列丢=" << decode_queue_dropped.load()
     << " 处理队列丢=" << dropped_frames.load()
     << " jitter=" << jitter_len_.load()
     << " 队列=" << queue_len_.load()
     << "\n信息 | 耗时均值 | "
     << "读取=" << mean(read_ms_) << "ms"
     << " 解码=" << mean(decode_ms_) << "ms"
     << " VPP=" << mean(vpp_ms_) << "ms"
     << " 归一化=" << mean(normalize_ms_) << "ms"
     << " 推理=" << mean(infer_ms_) << "ms"
     << " 后处理=" << mean(post_ms_) << "ms"
     << " 禁区=" << mean(region_ms_) << "ms"
     << " 绘制=" << mean(draw_ms_) << "ms"
     << " 编码=" << mean(encode_ms_) << "ms"
     << " RTMP=" << mean(rtmp_ms_) << "ms"
     << " 到达间隔P95/P99=" << p95(interarrival_ms_)
     << "/" << percentile(interarrival_ms_, 0.99) << "ms"
     << "\n信息 | 耗时P95 | "
     << "推理P95=" << p95(infer_ms_) << "ms"
     << " 编码P95=" << p95(encode_ms_) << "ms"
     << " e2eP95=" << p95(e2e_ms_) << "ms"
     << " e2e最大=" << (e2e_ms_.empty() ? 0.0 : *std::max_element(e2e_ms_.begin(), e2e_ms_.end()))
     << "ms";
  return os.str();
}

void PipelineMetrics::reset() {
  read_frames = 0; read_success = 0; decoded_frames = 0; scheduled_output = 0;
  queued_frames = 0; decode_queue_dropped = 0; dropped_frames = 0;
  preprocess_count = 0; inference_count = 0; empty_detection = 0;
  encode_sent = 0; encode_packets = 0; output_frames = 0; rtmp_write_fail = 0;
  rtsp_reconnects = 0; rtmp_reconnects = 0;
  rtsp_reconnect_attempts = 0; rtsp_reconnect_failures = 0;
  rtmp_reconnect_attempts = 0; rtmp_reconnect_failures = 0;
  last_read_ms = 0;
  std::lock_guard<std::mutex> lk(m_);
  read_ms_.clear(); interarrival_ms_.clear(); decode_ms_.clear();
  vpp_ms_.clear(); normalize_ms_.clear();
  pre_ms_.clear(); infer_ms_.clear(); post_ms_.clear(); region_ms_.clear();
  draw_ms_.clear(); encode_ms_.clear(); rtmp_ms_.clear(); e2e_ms_.clear();
  queue_len_ = 0; jitter_len_ = 0;
}

}  // namespace hzw
