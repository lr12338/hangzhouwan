// -*- coding: utf-8 -*-
// =============================================================================
// 视频健康判定纯逻辑（不依赖网络/硬件/FFmpeg，可独立单元测试）。
//
// 由 DualStreamApplication::metrics_loop 采集 A/B 两路原始指标后调用，计算：
//   - 整体 status：HEALTHY | DEGRADED | FAILED
//   - degradation：none | stream_degraded | stream_down | detection_only | resource_fatal
//   - 单路 level：HEALTHY | DEGRADED | FAILED
//
// 判定规则（对应运维文档 6.5 健康检查门禁，任一持续达到阈值即降级/失败）：
//   资源致命(VPU/gmem/解码器/编码器 ENOMEM) -> 该路 FAILED
//   RTSP 断开(最近帧超阈值未更新)         -> 该路 FAILED
//   output_fps 低于阈值                    -> 该路 DEGRADED
//   RTMP 断开                              -> 该路 DEGRADED
//   inference_fps 持续为 0(已连接)         -> 该路 DEGRADED
//   重连次数短时异常增长                   -> 该路 DEGRADED
//
// 整体：任一路 resource_fatal -> FAILED；双路均 FAILED -> FAILED；
//       单路 FAILED 或任一路 DEGRADED -> DEGRADED；业务仅检测 -> DEGRADED；否则 HEALTHY。
// =============================================================================
#ifndef HZW_MONITORING_VIDEO_HEALTH_LOGIC_H
#define HZW_MONITORING_VIDEO_HEALTH_LOGIC_H

#include <cstdint>
#include <string>

namespace hzw {

enum class StreamHealthLevel { HEALTHY, DEGRADED, FAILED };

// 单路健康输入（由调用方从 PipelineMetrics + pipeline.resource_fatal() 采集）。
struct StreamHealthInput {
  bool rtsp_connected = false;      // 最近帧在阈值内更新（now - last_read_ms < stale）
  bool rtmp_connected = false;      // 近期有成功输出帧
  double output_fps = 0.0;          // 本采样窗口输出帧率
  double inference_fps = 0.0;       // 本采样窗口推理帧率
  bool resource_fatal = false;      // VPU/解码器/编码器资源致命
  int64_t reconnect_delta = 0;      // 自上次采样以来重连增量（RTSP+RTMP）
};

// 健康阈值（可通过配置覆盖，默认值匹配生产验收）。
struct HealthThresholds {
  double min_output_fps = 5.0;          // 低于此值视为输出降级
  int64_t reconnect_storm_delta = 3;    // 单采样窗口重连增量超此视为重连风暴
};

// 单路健康等级。
StreamHealthLevel compute_stream_level(const StreamHealthInput& s,
                                       const HealthThresholds& t);

const char* level_str(StreamHealthLevel l);

// 双路整体健康结果。
struct DualHealthResult {
  std::string status;        // HEALTHY | DEGRADED | FAILED
  std::string degradation;   // none | stream_degraded | stream_down | detection_only | resource_fatal
  StreamHealthLevel level_a = StreamHealthLevel::HEALTHY;
  StreamHealthLevel level_b = StreamHealthLevel::HEALTHY;
  std::string reason;        // 人类可读原因（供健康接口/日志）
};

// 计算双路整体健康。business_full=false 表示业务降级为仅检测（无坐标/AIS）。
DualHealthResult compute_dual_health(const StreamHealthInput& a,
                                     const StreamHealthInput& b,
                                     bool business_full,
                                     const HealthThresholds& t);

}  // namespace hzw

#endif  // HZW_MONITORING_VIDEO_HEALTH_LOGIC_H
