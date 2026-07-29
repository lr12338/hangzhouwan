// -*- coding: utf-8 -*-
#include "video/rtsp_source_options.h"

#include <algorithm>
#include <cstdlib>

namespace hzw {

bool RtspSourceOptions::validate(std::string& err) const {
  if (transport != "tcp" && transport != "udp") {
    err = "rtsp_transport 必须为 tcp 或 udp";
    return false;
  }
  if (stimeout_us < 0) {
    err = "stimeout_us 非法（须 >= 0）";
    return false;
  }
  if (initial_backoff_ms <= 0) {
    err = "initial_backoff_ms 须 > 0";
    return false;
  }
  if (max_backoff_ms < initial_backoff_ms) {
    err = "max_backoff_ms 须 >= initial_backoff_ms";
    return false;
  }
  if (max_reconnect_attempts < -1) {
    err = "max_reconnect_attempts 须 >= -1";
    return false;
  }
  return true;
}

std::string redact_url_credentials(const std::string& url) {
  // 仅处理含 scheme://...@host 的 URL；其余原样返回，绝不抛异常。
  const std::string sep = "://";
  size_t scheme_end = url.find(sep);
  if (scheme_end == std::string::npos) return url;
  size_t userinfo_start = scheme_end + sep.size();
  size_t at = url.find('@', userinfo_start);
  if (at == std::string::npos) return url;  // 无凭据
  std::string userinfo = url.substr(userinfo_start, at - userinfo_start);
  size_t colon = userinfo.find(':');
  std::string masked;
  if (colon == std::string::npos) {
    // 仅有用户名、无密码：保留用户名，不强制添加密码占位。
    masked = userinfo;
  } else {
    masked = userinfo.substr(0, colon) + ":***";
  }
  return url.substr(0, userinfo_start) + masked + url.substr(at);
}

bool resolve_input_env(const std::string& env_name, std::string& out, std::string& err) {
  if (env_name.empty()) {
    err = "环境变量名为空";
    return false;
  }
  const char* val = std::getenv(env_name.c_str());
  if (val == nullptr || val[0] == '\0') {
    err = "环境变量 " + env_name + " 未设置";
    return false;
  }
  out = val;
  return true;
}

ReconnectPolicy::ReconnectPolicy(int64_t initial_ms, int64_t max_ms)
    : initial_ms_(initial_ms <= 0 ? 1 : initial_ms),
      max_ms_(max_ms < initial_ms_ ? initial_ms_ : max_ms) {}

int64_t ReconnectPolicy::on_failure() {
  ++attempts_;
  // 指数退避：initial * 2^(attempt-1)，封顶 max_ms_。attempt 上限保护防溢出。
  int64_t backoff = initial_ms_;
  int exp = attempts_ - 1;
  const int MAX_EXP = 30;  // 2^30 已远超任何毫秒封顶
  if (exp > MAX_EXP) exp = MAX_EXP;
  for (int i = 0; i < exp; ++i) {
    if (backoff > max_ms_ / 2) break;  // 再翻倍将超封顶，提前停
    backoff *= 2;
  }
  if (backoff > max_ms_) backoff = max_ms_;
  last_backoff_ms_ = backoff;
  return backoff;
}

void ReconnectPolicy::on_success() {
  attempts_ = 0;
  last_backoff_ms_ = 0;
}

}  // namespace hzw
