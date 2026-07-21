// -*- coding: utf-8 -*-
// RTSP 源选项单元测试：URL 凭据脱敏、环境变量解析、传输校验、重连退避策略。
// 全部为纯逻辑，不依赖网络与硬件。
#include <cstdlib>
#include <iostream>
#include <string>
#include "video/rtsp_source_options.h"

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cerr << "失败 | " << #cond << " @行" << __LINE__ << "\n";    \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

int main() {
  using hzw::redact_url_credentials;
  using hzw::resolve_input_env;
  using hzw::ReconnectPolicy;
  using hzw::RtspSourceOptions;

  // ---- 1) URL 凭据脱敏 ----
  {
    std::string r = redact_url_credentials("rtsp://admin:SecretPwd@192.168.1.10:554/stream");
    CHECK(r.find("SecretPwd") == std::string::npos);
    CHECK(r.find("admin") != std::string::npos);     // 保留用户名
    CHECK(r.find(":***@") != std::string::npos);
    CHECK(r.find("192.168.1.10:554") != std::string::npos);
  }
  // 仅用户名无密码：原样保留用户名，不抛异常。
  {
    std::string r = redact_url_credentials("rtsp://admin@192.168.1.10:554/stream");
    CHECK(r.find("admin@") != std::string::npos);
    CHECK(r.find("192.168.1.10") != std::string::npos);
  }
  // 无凭据：原样返回。
  {
    std::string u = "rtsp://192.168.1.10:554/stream";
    CHECK(redact_url_credentials(u) == u);
  }
  // 非 URL / 异常输入：原样返回，不抛异常。
  {
    CHECK(redact_url_credentials("not a url") == "not a url");
    CHECK(redact_url_credentials("") == "");
  }

  // ---- 2) 环境变量解析 ----
  {
    std::string out, err;
    CHECK(!resolve_input_env("", out, err));          // 空名
    CHECK(!err.empty());
  }
  {
    std::string out, err;
    CHECK(!resolve_input_env("HZW_TEST_RTSP_URL_NOT_SET_XYZ", out, err));  // 未设置
    CHECK(err.find("未设置") != std::string::npos);
    CHECK(out.empty());
  }
  {
    setenv("HZW_TEST_RTSP_URL_FAKE", "rtsp://u:p@127.0.0.1:8554/t", 1);
    std::string out, err;
    CHECK(resolve_input_env("HZW_TEST_RTSP_URL_FAKE", out, err));
    CHECK(out == "rtsp://u:p@127.0.0.1:8554/t");
    unsetenv("HZW_TEST_RTSP_URL_FAKE");
  }

  // ---- 3) RtspSourceOptions 校验 ----
  {
    RtspSourceOptions o; std::string err;
    CHECK(o.validate(err));                            // 默认 tcp 合法
  }
  {
    RtspSourceOptions o; o.transport = "http"; std::string err;
    CHECK(!o.validate(err));                           // 非法 transport
  }
  {
    RtspSourceOptions o; o.transport = "udp"; std::string err;
    CHECK(o.validate(err));                            // udp 合法
  }
  {
    RtspSourceOptions o; o.initial_backoff_ms = 0; std::string err;
    CHECK(!o.validate(err));
  }
  {
    RtspSourceOptions o; o.max_backoff_ms = 100; o.initial_backoff_ms = 1000; std::string err;
    CHECK(!o.validate(err));                           // max < initial
  }

  // ---- 4) 重连退避：指数增长、封顶、成功后重置 ----
  {
    ReconnectPolicy p(1000, 8000);
    CHECK(p.attempts() == 0);
    CHECK(p.on_failure() == 1000);   // 1000
    CHECK(p.on_failure() == 2000);   // 2000
    CHECK(p.on_failure() == 4000);   // 4000
    CHECK(p.on_failure() == 8000);   // 4000*2=8000
    CHECK(p.on_failure() == 8000);   // 封顶 8000
    CHECK(p.attempts() == 5);
    CHECK(p.last_backoff_ms() == 8000);
    p.on_success();                  // 成功重置
    CHECK(p.attempts() == 0);
    CHECK(p.last_backoff_ms() == 0);
    CHECK(p.on_failure() == 1000);   // 重置后从初始值开始
  }
  // max_reconnect_attempts 语义：max=0 不重连，max=2 允许 2 次。
  {
    RtspSourceOptions o; o.max_reconnect_attempts = 0; std::string err;
    CHECK(o.validate(err));
    o.max_reconnect_attempts = -1;
    CHECK(o.validate(err));          // 无限重连合法
  }

  if (g_failures == 0) std::cout << "通过 | RTSP 源选项单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
