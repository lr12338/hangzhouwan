// -*- coding: utf-8 -*-
// =============================================================================
// RTSP 源选项：连接/读取超时、受控重连退避、URL 凭据脱敏、环境变量安全输入。
//
// 仅使用 Sophon-FFmpeg 4.1.3-sophon-0.8.0 真实支持的 RTSP demuxer 选项
// （ffmpeg -h demuxer=rtsp 实测）：rtsp_transport、stimeout（socket TCP I/O 超时，
// 微秒）。禁止照搬新版 FFmpeg 或网络文章中的未验证选项。
//
// 全部为纯逻辑/字符串处理，不依赖网络与硬件，可独立单元测试。
// =============================================================================
#ifndef HZW_VIDEO_RTSP_SOURCE_OPTIONS_H
#define HZW_VIDEO_RTSP_SOURCE_OPTIONS_H

#include <cstdint>
#include <string>

namespace hzw {

// RTSP 连接与受控重连参数。
struct RtspSourceOptions {
  std::string transport = "tcp";      // tcp | udp（来自 ffmpeg -h demuxer=rtsp）
  int64_t stimeout_us = 5000000;      // socket TCP I/O 超时（微秒），覆盖连接与读取超时
  int max_reconnect_attempts = -1;    // -1=无限重连，0=不重连，>0=最多 N 次
  int64_t initial_backoff_ms = 1000;  // 初始退避
  int64_t max_backoff_ms = 30000;     // 指数退避上限
  // 校验合法性；返回 false 时 err 给出原因。
  bool validate(std::string& err) const;
};

// 脱敏 URL 凭据：rtsp://user:password@host:port/path -> rtsp://user:***@host:port/path
// 保留用户名便于排障，绝不输出密码；无凭据或异常输入原样返回，不抛异常。
std::string redact_url_credentials(const std::string& url);

// 从环境变量安全读取输入 URL。env_name 为空或变量未设置时返回 false（err 给出原因），
// 绝不打印 URL 值。调用方负责不在日志/命令行中输出返回值。
bool resolve_input_env(const std::string& env_name, std::string& out, std::string& err);

// 受控重连退避策略（指数退避，封顶 max_ms）。纯逻辑，不依赖网络/硬件/线程。
class ReconnectPolicy {
 public:
  ReconnectPolicy(int64_t initial_ms = 1000, int64_t max_ms = 30000);
  // 记录一次失败，返回下次应等待的退避毫秒（指数增长，封顶 max_ms）。
  int64_t on_failure();
  // 成功后重置 attempt 计数与上次退避。
  void on_success();
  int attempts() const { return attempts_; }
  int64_t last_backoff_ms() const { return last_backoff_ms_; }

 private:
  int64_t initial_ms_;
  int64_t max_ms_;
  int attempts_ = 0;
  int64_t last_backoff_ms_ = 0;
};

}  // namespace hzw

#endif  // HZW_VIDEO_RTSP_SOURCE_OPTIONS_H
