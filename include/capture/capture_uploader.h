// -*- coding: utf-8 -*-
// =============================================================================
// CaptureUploader：将 ready/ 目录下的 JPEG 上传到 HTTP 接收端（P6，可选）。
//
// 设计（见 goal-objective P6/十七）：
//   - 独立线程轮询 ready/，上传成功后移到 diagnostics/（或按 retention 清理），
//     失败重试有界次数后移到 failed/。
//   - 默认禁用（HTTP 上传不属于第一阶段核心目标）。
//   - 不阻塞抓拍引擎；上传失败不影响抓拍落盘。
//   - 不引入重依赖：使用 POSIX socket + 手写最小 HTTP/1.1 POST（curl 风格），
//     避免 libcurl 依赖；如需 HTTPS/复杂鉴权再扩展。
// =============================================================================
#ifndef HZW_CAPTURE_CAPTURE_UPLOADER_H
#define HZW_CAPTURE_CAPTURE_UPLOADER_H

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace hzw {

struct UploaderConfig {
  bool enabled = false;
  std::string ready_dir;        // 监听目录（ready/）
  std::string failed_dir;       // 上传失败归档
  std::string diagnostics_dir;  // 上传成功归档（可选保留）
  std::string http_host = "127.0.0.1";
  int http_port = 8080;
  std::string http_path = "/api/captures";
  int poll_interval_sec = 5;
  int max_retries = 3;
  int retention_diagnostic_files = 100;  // diagnostics 保留文件数（0=不限）
  // 上传后是否归档到 diagnostics（false=直接删除）
  bool archive_on_success = false;
};

class CaptureUploader {
 public:
  CaptureUploader();
  ~CaptureUploader();
  CaptureUploader(const CaptureUploader&) = delete;
  CaptureUploader& operator=(const CaptureUploader&) = delete;

  void start(const UploaderConfig& cfg);
  void stop();
  bool running() const { return running_.load(); }

  // 单次上传一个文件（供测试/CLI 直接调用）。成功返回 true。
  static bool upload_one(const std::string& filepath, const UploaderConfig& cfg,
                         std::string& err);

  // 列出 ready_dir 下的 .jpg 文件（供测试）。
  static int list_ready(const std::string& ready_dir, std::vector<std::string>& out);

 private:
  void loop();
  UploaderConfig cfg_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace hzw

#endif  // HZW_CAPTURE_CAPTURE_UPLOADER_H
