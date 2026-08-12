// -*- coding: utf-8 -*-
#include "capture/capture_uploader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hzw {

CaptureUploader::CaptureUploader() = default;
CaptureUploader::~CaptureUploader() { stop(); }

void CaptureUploader::start(const UploaderConfig& cfg) {
  cfg_ = cfg;
  if (!cfg_.enabled) return;
  running_.store(true);
  stop_.store(false);
  thread_ = std::thread(&CaptureUploader::loop, this);
}

void CaptureUploader::stop() {
  stop_.store(true);
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

int CaptureUploader::list_ready(const std::string& ready_dir,
                                std::vector<std::string>& out) {
  out.clear();
  DIR* d = opendir(ready_dir.c_str());
  if (!d) return 0;
  int n = 0;
  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    std::string name = ent->d_name;
    if (name.size() > 4 && name.substr(name.size() - 4) == ".jpg") {
      out.push_back(ready_dir + "/" + name);
      ++n;
    }
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return n;
}

bool CaptureUploader::upload_one(const std::string& filepath, const UploaderConfig& cfg,
                                 std::string& err) {
  // 读文件
  std::ifstream ifs(filepath, std::ios::binary);
  if (!ifs) { err = "无法打开文件"; return false; }
  std::string body((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  ifs.close();

  // 建立 TCP 连接
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  char port_str[16];
  std::snprintf(port_str, sizeof(port_str), "%d", cfg.http_port);
  if (getaddrinfo(cfg.http_host.c_str(), port_str, &hints, &res) != 0 || !res) {
    err = "DNS/连接失败: " + cfg.http_host;
    return false;
  }
  int sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock < 0) { freeaddrinfo(res); err = "socket 创建失败"; return false; }
  if (::connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
    ::close(sock); freeaddrinfo(res);
    err = "连接失败 " + cfg.http_host + ":" + std::to_string(cfg.http_port);
    return false;
  }
  freeaddrinfo(res);

  // 构造 multipart/form-data 或简单 PUT。这里用 application/octet-stream PUT（最薄）。
  std::string filename = filepath.substr(filepath.find_last_of('/') + 1);
  std::string header =
      "PUT " + cfg.http_path + "/" + filename + " HTTP/1.1\r\n"
      "Host: " + cfg.http_host + "\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: " + std::to_string(body.size()) + "\r\n"
      "Connection: close\r\n\r\n";
  std::string request = header + body;

  ssize_t sent = 0;
  while (sent < (ssize_t)request.size()) {
    ssize_t w = ::send(sock, request.data() + sent, request.size() - sent, 0);
    if (w <= 0) { ::close(sock); err = "发送失败"; return false; }
    sent += w;
  }

  // 读响应状态行
  char resp[256] = {};
  ssize_t r = ::recv(sock, resp, sizeof(resp) - 1, 0);
  ::close(sock);
  if (r <= 0) { err = "无响应"; return false; }
  // 解析 "HTTP/1.1 200 ..."
  int status = 0;
  std::sscanf(resp, "HTTP/%*s %d", &status);
  if (status >= 200 && status < 300) return true;
  err = "HTTP " + std::to_string(status);
  return false;
}

static void move_file(const std::string& src, const std::string& dst_dir) {
  std::string filename = src.substr(src.find_last_of('/') + 1);
  std::string dst = dst_dir + "/" + filename;
  if (std::rename(src.c_str(), dst.c_str()) != 0) {
    // 跨设备 rename 失败则复制+删除
    std::ifstream ifs(src, std::ios::binary);
    std::ofstream ofs(dst, std::ios::binary);
    ofs << ifs.rdbuf();
    ifs.close(); ofs.close();
    ::unlink(src.c_str());
  }
}

static void cleanup_dir(const std::string& dir, int max_files) {
  if (max_files <= 0) return;
  std::vector<std::string> files;
  CaptureUploader::list_ready(dir, files);  // 已排序
  // list_ready 过滤 .jpg；diagnostics 也存 metadata，此处只数 .jpg
  while ((int)files.size() > max_files) {
    ::unlink(files.front().c_str());
    files.erase(files.begin());
  }
}

void CaptureUploader::loop() {
  while (!stop_.load()) {
    std::vector<std::string> files;
    list_ready(cfg_.ready_dir, files);
    for (const auto& f : files) {
      if (stop_.load()) break;
      std::string err;
      bool ok = false;
      for (int attempt = 0; attempt < cfg_.max_retries && !stop_.load(); ++attempt) {
        ok = upload_one(f, cfg_, err);
        if (ok) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      if (ok) {
        if (cfg_.archive_on_success) {
          move_file(f, cfg_.diagnostics_dir);
          cleanup_dir(cfg_.diagnostics_dir, cfg_.retention_diagnostic_files);
        } else {
          ::unlink(f.c_str());  // 上传成功直接删除
        }
        std::fprintf(stdout, "信息 | uploader | 上传成功 %s\n", f.c_str());
      } else {
        move_file(f, cfg_.failed_dir);
        std::fprintf(stderr, "警告 | uploader | 上传失败 %s: %s\n", f.c_str(), err.c_str());
      }
    }
    for (int i = 0; i < cfg_.poll_interval_sec * 10 && !stop_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

}  // namespace hzw
