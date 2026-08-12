// -*- coding: utf-8 -*-
// CaptureUploader 纯逻辑测试（list_ready + 文件归档/清理）。无网络依赖。
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "capture/capture_uploader.h"

static int failures = 0;
#define CHECK(cond) do { if(!(cond)){ std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while(0)

static void write_file(const std::string& path, const std::string& content) {
  std::ofstream ofs(path, std::ios::binary);
  ofs << content;
}

int main() {
  // 用 /tmp 创建临时目录结构
  std::string base = "/tmp/hzw_upload_test";
  std::string ready = base + "/ready";
  std::string failed = base + "/failed";
  std::string diag = base + "/diagnostics";
  std::string cmd = "mkdir -p " + ready + " " + failed + " " + diag;
  std::system(cmd.c_str());

  // 写几个 jpg + 非 jpg
  write_file(ready + "/north_001_20260812.jpg", "JPEGDATA1");
  write_file(ready + "/south_002_20260812.jpg", "JPEGDATA2");
  write_file(ready + "/notimage.txt", "text");
  write_file(ready + "/north_003_20260812.jpg", "JPEGDATA3");

  // list_ready 只返回 .jpg
  std::vector<std::string> files;
  int n = hzw::CaptureUploader::list_ready(ready, files);
  CHECK(n == 3);
  CHECK(files.size() == 3);
  // 已排序
  CHECK(files[0].find("north_001") != std::string::npos);
  CHECK(files[1].find("north_003") != std::string::npos);
  CHECK(files[2].find("south_002") != std::string::npos);

  // upload_one 到不存在的服务器应失败但不崩溃
  hzw::UploaderConfig cfg;
  cfg.http_host = "127.0.0.1";
  cfg.http_port = 1;  // 不可达端口
  cfg.http_path = "/api/captures";
  std::string err;
  bool ok = hzw::CaptureUploader::upload_one(files[0], cfg, err);
  CHECK(!ok);
  CHECK(!err.empty());

  // 清理
  std::system(("rm -rf " + base).c_str());

  if (failures == 0) { std::printf("capture_uploader: all passed\n"); return 0; }
  std::printf("capture_uploader: %d failures\n", failures);
  return 1;
}
