// -*- coding: utf-8 -*-
// =============================================================================
// 视频健康判定纯逻辑（不依赖网络/硬件/FFmpeg，可独立单元测试）。
//
// 由 DualStreamApplication::metrics_loop 采集 A/B 两路原始指标后调用，计算：
//   - 整体 status：HEALTHY | DEGRADED | FAILED
//   - degradation：none | stream_degraded | stream_down | detection_only | resource_fatal
//   - 单路 level：HEALTHY | DEGRADED | FAILED
//
// 判定规则（对应运维门禁 6.5，避免过度判死与状态抖动）：
//   资源致命(VPU/gmem/解码器/编码器 ENOMEM)       -> 该路 FAILED（立即，最高优先级）
//   RTSP 断开超 reconnect_grace_ms(持续断流)      -> 该路 FAILED
//   RTSP 断开在 frame_stale_ms~grace_ms(瞬时/重连中) -> 该路 DEGRADED（宽限期，不立即判死）
//   output_fps 低于阈值                            -> 该路 DEGRADED
//   RTMP 断开                                      -> 该路 DEGRADED
//   inference_fps 持续为 0(已连接)                 -> 该路 DEGRADED
//   重连次数短时异常增长(重连风暴)                 -> 该路 DEGRADED
//
// 防抖(StreamHealthDebouncer)：
//   - 瞬时等级为 FAILED 时立即提交（关键错误/持续断流，不过度延迟告警）；
//   - HEALTHY->DEGRADED 需 confirm_down 次连续确认（滤除单采样毛刺）；
//   - 降级->恢复(HEALTHY)需 confirm_up 次连续确认（避免恢复抖动/假恢复）。
//
// 整体：任一路 resource_fatal -> FAILED；双路均 FAILED -> FAILED；
//       单路 FAILED -> DEGRADED(突出故障流)；任一路 DEGRADED -> DEGRADED；
//       业务仅检测 -> DEGRADED；否则 HEALTHY。systemd active 但数据面失败绝不显示 HEALTHY。
//
// 所有阈值集中于 HealthThresholds 并写明默认值；调用方可覆盖（生产路径用默认值）。
// =============================================================================
#ifndef HZW_MONITORING_VIDEO_HEALTH_LOGIC_H
#define HZW_MONITORING_VIDEO_HEALTH_LOGIC_H

#include <cstdint>
#include <string>

namespace hzw {

enum class StreamHealthLevel { HEALTHY, DEGRADED, FAILED };

// 单路健康输入（由调用方从 PipelineMetrics + pipeline.resource_fatal() 采集）。
struct StreamHealthInput {
  int64_t disconnected_ms = 0;  // 自上次成功读取帧以来的毫秒数（0=刚读到帧）；last_read_ms==0 时调用方传 0
  bool rtmp_connected = false;  // 近期有成功输出帧
  double output_fps = 0.0;      // 本采样窗口输出帧率
  double inference_fps = 0.0;   // 本采样窗口推理帧率
  bool resource_fatal = false;  // VPU/解码器/编码器资源致命
  int64_t reconnects_1h = 0;    // 滑动一小时内重连次数（RTSP+RTMP）
};

// 健康阈值（集中定义，默认值匹配生产验收；调用方可构造自定义值覆盖）。
struct HealthThresholds {
  int64_t frame_stale_ms = 15000;       // 超过此值视为 RTSP 断开（最近帧过时）
  int64_t reconnect_grace_ms = 60000;   // 断开宽限期：[stale,grace) 为瞬时重连(DEGRADED)，>=grace 为持续断流(FAILED)
  double min_output_fps = 7.0;          // 低于此值视为输出降级
  double min_inference_fps = 4.0;       // 低于此值视为推理降级
  int64_t max_reconnects_per_hour = 2;  // 滑动一小时重连上限
  int confirm_down = 2;                 // HEALTHY->DEGRADED 需连续确认采样数（防毛刺）
  int confirm_up = 3;                   // 降级->HEALTHY 恢复需连续确认采样数（防抖）
};

// 单路瞬时健康等级（无状态，每次采样计算）。
StreamHealthLevel compute_stream_level(const StreamHealthInput& s,
                                       const HealthThresholds& t);

const char* level_str(StreamHealthLevel l);

// 单路健康防抖器（有状态，跨采样保持，用于抑制状态抖动）。
// 语义：瞬时 FAILED 立即提交；其余变化需连续 confirm 次确认。
class StreamHealthDebouncer {
 public:
  // 喂入本采样瞬时等级，返回防抖后提交等级。confirm_down/up 来自 HealthThresholds。
  StreamHealthLevel update(StreamHealthLevel instantaneous,
                           int confirm_down, int confirm_up);
  StreamHealthLevel committed() const { return committed_; }
  void reset() { committed_ = StreamHealthLevel::HEALTHY; candidate_ = StreamHealthLevel::HEALTHY; streak_ = 0; }

 private:
  StreamHealthLevel committed_ = StreamHealthLevel::HEALTHY;
  StreamHealthLevel candidate_ = StreamHealthLevel::HEALTHY;
  int streak_ = 0;
};

// 双路整体健康结果。
struct DualHealthResult {
  std::string status;        // HEALTHY | DEGRADED | FAILED
  std::string degradation;   // none | stream_degraded | stream_down | detection_only | resource_fatal
  StreamHealthLevel level_a = StreamHealthLevel::HEALTHY;
  StreamHealthLevel level_b = StreamHealthLevel::HEALTHY;
  std::string reason;        // 人类可读原因（供健康接口/日志）
};

struct BusinessHealthResult {
  std::string link;  // HEALTHY | FAILED | DISABLED
  std::string mode;  // PENDING | FULL | COORD_ONLY | DETECTION_ONLY
  bool link_healthy = true;
};

BusinessHealthResult compute_business_health(
    bool enabled_a, bool link_a, const std::string& mode_a,
    bool enabled_b, bool link_b, const std::string& mode_b);

// 计算双路整体健康（基于已防抖的单路等级）。fatal_a/b 用于区分 resource_fatal 降级类别。
// business_link_healthy=false 表示 Sidecar 链路不可用。本帧无目标/PENDING 不降级。
DualHealthResult compute_dual_status(StreamHealthLevel level_a,
                                     StreamHealthLevel level_b,
                                     bool fatal_a, bool fatal_b,
                                     bool business_link_healthy);

}  // namespace hzw

#endif  // HZW_MONITORING_VIDEO_HEALTH_LOGIC_H
