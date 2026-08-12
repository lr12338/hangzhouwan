// -*- coding: utf-8 -*-
// =============================================================================
// Bridge Capture UDS 协议（4 字节大端长度前缀 + UTF-8 JSON）。
//
// 与 bridge_crossing.control.ControlServer 同一 framing；capture 侧独立 socket：
//   /run/hangzhouwan/bridge-capture.sock
//
// 命令（bridge -> capture）：
//   arm    : 启动抓拍会话 -> 立即返回 {accepted,state:QUEUED}（禁止阻塞等抓拍完成）
//   status : 查询当前/最近会话状态
//   cancel : 取消会话
//   health : 健康检查
//
// 协议帧编解码纯逻辑，无硬件/网络依赖，可单元测试。
// JSON 序列化使用轻量手写解析（避免引入 nlohmann/json 等重依赖；命令字段固定）。
// =============================================================================
#ifndef HZW_CAPTURE_CAPTURE_PROTOCOL_H
#define HZW_CAPTURE_CAPTURE_PROTOCOL_H

#include <cstdint>
#include <string>

namespace hzw {

// 4B 大端长度 + payload。返回完整帧字节。
std::string encode_frame(const std::string& json_payload);

// 从缓冲区解码一帧。成功返回 true 并消费 [0,consumed) 字节。
// 不完整返回 false（consumed=0）；长度非法返回 false 且 error=true。
bool decode_frame(const std::string& buf, std::string& out_payload,
                  size_t& consumed, bool& error);

// 最大 payload（4 MiB，与 control 一致）。
constexpr size_t kCaptureMaxPayload = 4 * 1024 * 1024;

// ---- 轻量 JSON 值提取（仅支持扁平对象 + 字符串/数字/布尔字段）----
// 不做完整 JSON 解析；仅按 key 提取命令所需字段，避免依赖外部库。
std::string json_get_string(const std::string& json, const std::string& key);
int64_t json_get_int(const std::string& json, const std::string& key, int64_t def = 0);
double json_get_double(const std::string& json, const std::string& key, double def = 0.0);
bool json_get_bool(const std::string& json, const std::string& key, bool def = false);

// 构造 arm 命令 JSON（bridge -> capture）。
std::string make_arm_json(const std::string& session_id, const std::string& bridge,
                          const std::string& mmsi, const std::string& direction,
                          int64_t trigger_ts_ms, double distance_to_gate_m,
                          double eta_sec, int timeout_sec);

// 构造 arm 响应 JSON。
std::string make_arm_response(bool accepted, const std::string& session_id,
                              const std::string& state, const std::string& reason = "");

// 构造 status/health 响应 JSON。
std::string make_status_json(const std::string& state, const std::string& bridge,
                             const std::string& session_id, const std::string& jpeg_path,
                             int active_sessions, int64_t captured_total);
std::string make_health_json(bool healthy, int64_t uptime_s, int active_sessions,
                             int64_t captured_total, const std::string& bmodel_path);

}  // namespace hzw

#endif  // HZW_CAPTURE_CAPTURE_PROTOCOL_H
