// -*- coding: utf-8 -*-
// =============================================================================
// bridge_capture_app：AIS 驱动的南北通航孔船舶抓拍服务（独立 executable）。
//
// 复用 hzw_inf（SophonVideoSource/BmrtDetector/BmcvProcessor/YOLO/JPEG），不复制源码。
// 启动：加载 1×bmodel -> 启动 UDS server(/run/hangzhouwan/bridge-capture.sock) -> IDLE。
// 收到 arm -> 异步开对应视频 -> 推理 -> 最佳帧 -> crop -> JPEG -> 回到 IDLE。
//
// 手工测试（P2）：
//   bridge_capture_app --bmodel weights/yolov7.bmodel --device 0 \
//     --capture-dir /data/hangzhouwan/bridge/captures \
//     --north-url-env CAPTURE_NORTH_URL --south-url-env CAPTURE_SOUTH_URL
//   # arm（CLI 也可不经 UDS 直接 arm 测试）：
//   bridge_capture_app --cli-arm north --mmsi 414402810 --direction upstream \
//     --bmodel ... --north-url-env CAPTURE_NORTH_URL
// =============================================================================
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#include "capture/capture_engine.h"
#include "capture/capture_protocol.h"

namespace {

hzw::CaptureEngine* g_engine = nullptr;

void on_signal(int sig) {
  std::fprintf(stdout, "\n信息 | 信号 %d，停止 capture\n", sig);
  std::fflush(stdout);
  if (g_engine) g_engine->stop();
}

std::string get_env(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

// 解析 bridge capture 配置（north/south 共享默认 ROI）。
hzw::BridgeCaptureConfig make_bridge_cfg(const std::string& bridge,
                                         const std::string& url_env,
                                         int fps) {
  hzw::BridgeCaptureConfig bc;
  bc.bridge = bridge;
  bc.url_env = url_env;
  bc.inference_fps = fps;
  bc.inference_fps_shared = std::max(2, fps - 2);
  // ROI 默认全图；生产按摄像头实际校准（P5）。
  // valid_roi / capture_roi 留默认 0~1。
  return bc;
}

void print_usage() {
  std::fprintf(stdout,
      "用法: bridge_capture_app [选项]\n"
      "  --bmodel PATH           bmodel 路径（必需）\n"
      "  --device N              BM 设备号（默认 0）\n"
      "  --capture-dir PATH      抓拍输出目录（默认 /data/hangzhouwan/bridge/captures）\n"
      "  --socket PATH           UDS 路径（默认 /run/hangzhouwan/bridge-capture.sock）\n"
      "  --north-url-env NAME    北通航孔 RTSP URL 环境变量名\n"
      "  --south-url-env NAME    南通航孔 RTSP URL 环境变量名\n"
      "  --fps N                 单路推理 FPS（默认 5）\n"
      "  --cli-arm BRIDGE        手工 ARM 测试（north/south），不走 UDS\n"
      "  --mmsi MMSI             手工 ARM 时的 MMSI\n"
      "  --direction DIR         upstream|downstream\n"
      "  --timeout SEC           会话超时（默认 180）\n"
      "  --help                  显示帮助\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string bmodel, capture_dir = "/data/hangzhouwan/bridge/captures";
  std::string socket_path = "/run/hangzhouwan/bridge-capture.sock";
  std::string north_env, south_env, cli_arm, mmsi, direction = "upstream";
  int device = 0, fps = 5, timeout_sec = 180;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 < argc) return argv[++i];
      std::fprintf(stderr, "错误 | %s 缺少参数\n", name);
      std::exit(2);
    };
    if (a == "--help" || a == "-h") { print_usage(); return 0; }
    else if (a == "--bmodel") bmodel = next("--bmodel");
    else if (a == "--device") device = std::atoi(next("--device").c_str());
    else if (a == "--capture-dir") capture_dir = next("--capture-dir");
    else if (a == "--socket") socket_path = next("--socket");
    else if (a == "--north-url-env") north_env = next("--north-url-env");
    else if (a == "--south-url-env") south_env = next("--south-url-env");
    else if (a == "--fps") fps = std::atoi(next("--fps").c_str());
    else if (a == "--cli-arm") cli_arm = next("--cli-arm");
    else if (a == "--mmsi") mmsi = next("--mmsi");
    else if (a == "--direction") direction = next("--direction");
    else if (a == "--timeout") timeout_sec = std::atoi(next("--timeout").c_str());
    else { std::fprintf(stderr, "未知参数: %s\n", a.c_str()); print_usage(); return 2; }
  }

  if (bmodel.empty()) { std::fprintf(stderr, "错误 | --bmodel 必需\n"); print_usage(); return 2; }

  hzw::CaptureEngineConfig cfg;
  cfg.device = device;
  cfg.bmodel_path = bmodel;
  cfg.capture_dir = capture_dir;
  cfg.socket_path = socket_path;
  cfg.north = make_bridge_cfg("north", north_env, fps);
  cfg.north.session_timeout_sec = timeout_sec;
  cfg.south = make_bridge_cfg("south", south_env, fps);
  cfg.south.session_timeout_sec = timeout_sec;

  hzw::CaptureEngine engine;
  g_engine = &engine;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::string err;
  if (!engine.init(cfg, err)) {
    std::fprintf(stderr, "错误 | capture 初始化失败: %s\n", err.c_str());
    return 70;  // kPipelineExitHardware
  }

  // 手工 ARM CLI 模式（P2 板端验证）：直接 arm，同步等待结果。
  if (!cli_arm.empty()) {
    hzw::CaptureArmRequest req;
    req.session_id = "cli-" + std::to_string(std::time(nullptr));
    req.bridge = cli_arm;
    req.mmsi = mmsi.empty() ? "000000000" : mmsi;
    req.direction = direction;
    req.trigger_ts_ms = 0;
    req.timeout_sec = timeout_sec;
    std::string resp = engine.handle_arm(req);
    std::fprintf(stdout, "ARM 响应: %s\n", resp.c_str());
    std::fflush(stdout);
    // 等待会话完成（轮询状态）
    for (int i = 0; i < timeout_sec * 2; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      std::string st = engine.handle_status();
      if (std::strstr(st.c_str(), "\"state\":\"IDLE\"")) {
        std::fprintf(stdout, "完成: %s\n", st.c_str());
        break;
      }
    }
    std::fprintf(stdout, "最终状态: %s\n", engine.handle_status().c_str());
    return 0;
  }

  // UDS server 模式
  // 创建 socket 目录
  std::string sock_dir = socket_path.substr(0, socket_path.find_last_of('/'));
  if (!sock_dir.empty()) {
    std::string mk = "mkdir -p " + sock_dir;
    std::system(mk.c_str());
  }
  ::unlink(socket_path.c_str());
  int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (srv < 0) { std::fprintf(stderr, "错误 | socket 创建失败\n"); return 1; }
  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  if (::bind(srv, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::fprintf(stderr, "错误 | bind %s 失败\n", socket_path.c_str());
    return 1;
  }
  ::chmod(socket_path.c_str(), 0660);
  ::listen(srv, 8);
  std::fprintf(stdout, "信息 | capture | UDS server 监听 %s\n", socket_path.c_str());
  std::fflush(stdout);

  while (true) {
    if (engine.resource_fatal()) {
      std::fprintf(stderr, "错误 | 资源致命，退出\n");
      ::close(srv);
      ::unlink(socket_path.c_str());
      return 70;
    }
    int conn = ::accept(srv, nullptr, nullptr);
    if (conn < 0) {
      if (errno == EINTR) continue;
      continue;
    }
    // 读一帧（4B 长度 + payload）
    char hdr[4];
    ssize_t n = 0;
    while (n < 4) {
      ssize_t r = ::read(conn, hdr + n, 4 - n);
      if (r <= 0) break;
      n += r;
    }
    if (n < 4) { ::close(conn); continue; }
    uint32_t len = (static_cast<uint8_t>(hdr[0]) << 24) |
                   (static_cast<uint8_t>(hdr[1]) << 16) |
                   (static_cast<uint8_t>(hdr[2]) << 8) |
                   static_cast<uint8_t>(hdr[3]);
    if (len == 0 || len > hzw::kCaptureMaxPayload) { ::close(conn); continue; }
    std::string payload(len, '\0');
    size_t got = 0;
    while (got < len) {
      ssize_t r = ::read(conn, &payload[got], len - got);
      if (r <= 0) break;
      got += r;
    }
    if (got < len) { ::close(conn); continue; }

    // 分发命令
    std::string cmd = hzw::json_get_string(payload, "cmd");
    std::string resp;
    if (cmd == "arm") {
      hzw::CaptureArmRequest req;
      req.session_id = hzw::json_get_string(payload, "session_id");
      req.bridge = hzw::json_get_string(payload, "bridge");
      req.mmsi = hzw::json_get_string(payload, "mmsi");
      req.direction = hzw::json_get_string(payload, "direction");
      req.trigger_ts_ms = hzw::json_get_int(payload, "trigger_ts_ms");
      req.distance_to_gate_m = hzw::json_get_double(payload, "distance_to_gate_m");
      req.eta_sec = hzw::json_get_double(payload, "eta_sec");
      req.timeout_sec = static_cast<int>(hzw::json_get_int(payload, "timeout_sec", 180));
      resp = engine.handle_arm(req);
    } else if (cmd == "status") {
      resp = engine.handle_status();
    } else if (cmd == "health") {
      resp = engine.handle_health();
    } else if (cmd == "cancel") {
      resp = engine.handle_cancel(hzw::json_get_string(payload, "session_id"));
    } else {
      resp = std::string("{\"error\":\"unknown cmd\",\"code\":-1}");
    }

    std::string frame = hzw::encode_frame(resp);
    if (!frame.empty()) ::write(conn, frame.data(), frame.size());
    ::close(conn);
  }
  return 0;
}
