// -*- coding: utf-8 -*-
#include "capture/capture_protocol.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace hzw {

static void put_u32_be(std::string& out, uint32_t v) {
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>(v & 0xFF));
}

std::string encode_frame(const std::string& json_payload) {
  if (json_payload.size() > kCaptureMaxPayload) {
    return {};
  }
  std::string out;
  out.reserve(4 + json_payload.size());
  put_u32_be(out, static_cast<uint32_t>(json_payload.size()));
  out += json_payload;
  return out;
}

bool decode_frame(const std::string& buf, std::string& out_payload,
                  size_t& consumed, bool& error) {
  consumed = 0;
  error = false;
  if (buf.size() < 4) return false;
  uint32_t len = (static_cast<uint8_t>(buf[0]) << 24) |
                 (static_cast<uint8_t>(buf[1]) << 16) |
                 (static_cast<uint8_t>(buf[2]) << 8) |
                 static_cast<uint8_t>(buf[3]);
  if (len == 0 || len > kCaptureMaxPayload) {
    error = true;
    return false;
  }
  if (buf.size() < 4 + len) return false;  // 不完整
  out_payload.assign(buf, 4, len);
  consumed = 4 + len;
  return true;
}

// ---- 轻量 JSON 提取 ----
static const char* find_key(const std::string& json, const std::string& key) {
  std::string pat = "\"" + key + "\"";
  size_t pos = json.find(pat);
  if (pos == std::string::npos) return nullptr;
  pos += pat.size();
  pos = json.find(':', pos);
  if (pos == std::string::npos) return nullptr;
  pos++;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                               json[pos] == '\n' || json[pos] == '\r')) pos++;
  return json.c_str() + pos;
}

std::string json_get_string(const std::string& json, const std::string& key) {
  const char* p = find_key(json, key);
  if (!p || *p != '"') return "";
  p++;
  std::string out;
  while (*p && *p != '"') {
    if (*p == '\\' && *(p + 1)) {
      p++;
      switch (*p) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        default: out += *p; break;
      }
    } else {
      out += *p;
    }
    p++;
  }
  return out;
}

int64_t json_get_int(const std::string& json, const std::string& key, int64_t def) {
  const char* p = find_key(json, key);
  if (!p) return def;
  if (*p == '"') return def;  // 字符串不是数字
  try {
    return std::stoll(p);
  } catch (...) {
    return def;
  }
}

double json_get_double(const std::string& json, const std::string& key, double def) {
  const char* p = find_key(json, key);
  if (!p) return def;
  if (*p == '"') return def;
  try {
    return std::stod(p);
  } catch (...) {
    return def;
  }
}

bool json_get_bool(const std::string& json, const std::string& key, bool def) {
  const char* p = find_key(json, key);
  if (!p) return def;
  if (std::strncmp(p, "true", 4) == 0) return true;
  if (std::strncmp(p, "false", 5) == 0) return false;
  return def;
}

// ---- 命令构造 ----
static std::string esc(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

std::string make_arm_json(const std::string& session_id, const std::string& bridge,
                          const std::string& mmsi, const std::string& direction,
                          int64_t trigger_ts_ms, double distance_to_gate_m,
                          double eta_sec, int timeout_sec) {
  std::ostringstream o;
  o << "{\"cmd\":\"arm\",\"session_id\":\"" << esc(session_id) << "\","
    << "\"bridge\":\"" << esc(bridge) << "\","
    << "\"mmsi\":\"" << esc(mmsi) << "\","
    << "\"direction\":\"" << esc(direction) << "\","
    << "\"trigger_ts_ms\":" << trigger_ts_ms << ","
    << "\"distance_to_gate_m\":" << distance_to_gate_m << ","
    << "\"eta_sec\":" << eta_sec << ","
    << "\"timeout_sec\":" << timeout_sec << "}";
  return o.str();
}

std::string make_arm_response(bool accepted, const std::string& session_id,
                              const std::string& state, const std::string& reason) {
  std::ostringstream o;
  o << "{\"accepted\":" << (accepted ? "true" : "false") << ","
    << "\"session_id\":\"" << esc(session_id) << "\","
    << "\"state\":\"" << esc(state) << "\"";
  if (!reason.empty()) o << ",\"reason\":\"" << esc(reason) << "\"";
  o << "}";
  return o.str();
}

std::string make_status_json(const std::string& state, const std::string& bridge,
                             const std::string& session_id, const std::string& jpeg_path,
                             int active_sessions, int64_t captured_total) {
  std::ostringstream o;
  o << "{\"state\":\"" << esc(state) << "\","
    << "\"bridge\":\"" << esc(bridge) << "\","
    << "\"session_id\":\"" << esc(session_id) << "\","
    << "\"jpeg_path\":\"" << esc(jpeg_path) << "\","
    << "\"active_sessions\":" << active_sessions << ","
    << "\"captured_total\":" << captured_total << "}";
  return o.str();
}

std::string make_health_json(bool healthy, int64_t uptime_s, int active_sessions,
                             int64_t captured_total, const std::string& bmodel_path) {
  std::ostringstream o;
  o << "{\"healthy\":" << (healthy ? "true" : "false") << ","
    << "\"uptime_s\":" << uptime_s << ","
    << "\"active_sessions\":" << active_sessions << ","
    << "\"captured_total\":" << captured_total << ","
    << "\"bmodel\":\"" << esc(bmodel_path) << "\"}";
  return o.str();
}

}  // namespace hzw
