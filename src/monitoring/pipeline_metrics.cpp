// -*- coding: utf-8 -*-
#include "monitoring/pipeline_metrics.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace hzw {

void PipelineMetrics::push(std::deque<double>& d, double v) {
  d.push_back(v);
  // 保留最近 2000 个采样，足够 P95 稳定且内存可控。
  if (d.size() > 2000) d.pop_front();
}

double PipelineMetrics::mean(const std::deque<double>& d) {
  if (d.empty()) return 0.0;
  double s = 0;
  for (double v : d) s += v;
  return s / static_cast<double>(d.size());
}

double PipelineMetrics::p95(const std::deque<double>& d) {
  if (d.empty()) return 0.0;
  std::vector<double> v(d.begin(), d.end());
  std::sort(v.begin(), v.end());
  size_t idx = static_cast<size_t>(v.size() * 0.95);
  if (idx >= v.size()) idx = v.size() - 1;
  return v[idx];
}

void PipelineMetrics::record_e2e_ms(double v) { push(e2e_ms_, v); }

std::string PipelineMetrics::summary() const {
  std::lock_guard<std::mutex> lk(m_);
  std::ostringstream os;
  os << "信息 | 视频管线 | "
     << "解码=" << decoded_frames.load()
     << " 输出=" << output_frames.load()
     << " 推理=" << inference_count.load()
     << " 空检=" << empty_detection.load()
     << " 丢帧=" << dropped_frames.load()
     << "\n信息 | 视频管线 | "
     << "解码均值=" << mean(decode_ms_) << "ms"
     << " 预处理均值=" << mean(pre_ms_) << "ms"
     << " 推理均值=" << mean(infer_ms_) << "ms"
     << " 编码均值=" << mean(encode_ms_) << "ms"
     << "\n信息 | 视频管线 | "
     << "延迟 当前=" << (e2e_ms_.empty() ? 0.0 : e2e_ms_.back())
     << "ms P95=" << p95(e2e_ms_) << "ms"
     << " 最大=" << (e2e_ms_.empty() ? 0.0 : *std::max_element(e2e_ms_.begin(), e2e_ms_.end()))
     << "ms 队列=" << queue_len_.load();
  return os.str();
}

void PipelineMetrics::reset() {
  read_frames = 0; decoded_frames = 0; queued_frames = 0; dropped_frames = 0;
  output_frames = 0; inference_count = 0; empty_detection = 0;
  std::lock_guard<std::mutex> lk(m_);
  decode_ms_.clear(); pre_ms_.clear(); infer_ms_.clear();
  post_ms_.clear(); encode_ms_.clear(); e2e_ms_.clear();
  queue_len_ = 0;
}

}  // namespace hzw
