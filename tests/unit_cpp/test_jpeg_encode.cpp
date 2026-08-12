// -*- coding: utf-8 -*-
// =============================================================================
// JPEG 编码 / 解码回归测试（纯逻辑，无需硬件）。
//
// 目的：
//   - 防止以后 CMake/link 改动让 encode_jpeg / decode_jpeg 静默失效；
//   - 覆盖 section 17 要求的最小集：RGB Image 1280x202 编码、解码回读尺寸。
//
// 链接：hzw_inf（已 PUBLIC 链接 JPEG::JPEG）。
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "image_io/jpeg_io.h"

namespace {
int failures = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { std::fprintf(stderr, "FAIL %s line=%d\n", msg, __LINE__); ++failures; } \
  else { std::printf("  [通过] %s\n", msg); } \
} while (0)

bool file_exists(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return f.good();
}

long file_size(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  return f.good() ? static_cast<long>(f.tellg()) : -1;
}

hzw::Image make_gradient(int w, int h) {
  hzw::Image img;
  img.width = w; img.height = h;
  img.data.assign(static_cast<size_t>(w) * h * hzw::Image::channels, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t* p = img.pixel(x, y);
      p[0] = static_cast<uint8_t>(x & 0xff);
      p[1] = static_cast<uint8_t>((y * 2) & 0xff);
      p[2] = static_cast<uint8_t>((x + y) & 0xff);
    }
  }
  return img;
}
}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  const std::string out_path = "/tmp/hzw_test_jpeg_encode.jpg";

  hzw::Image img = make_gradient(1280, 202);
  CHECK(img.valid(), "Image 1280x202 构造有效");
  bool enc_ok = hzw::encode_jpeg(out_path, img, 85);
  CHECK(enc_ok, "encode_jpeg 成功");

  CHECK(file_exists(out_path), "JPEG 文件存在");
  long sz = file_size(out_path);
  CHECK(sz > 0, "JPEG 文件 size > 0");
  if (sz > 0) std::printf("  [信息] 文件大小 = %ld 字节\n", sz);

  {
    std::ifstream f(out_path, std::ios::binary);
    unsigned char hdr[2] = {0, 0};
    f.read(reinterpret_cast<char*>(hdr), 2);
    CHECK(hdr[0] == 0xFF && hdr[1] == 0xD8, "JPEG 文件头 FF D8");
  }

  hzw::Image back;
  bool dec_ok = hzw::decode_jpeg(out_path, back);
  CHECK(dec_ok, "decode_jpeg 成功");
  CHECK(back.width == img.width && back.height == img.height, "decode 尺寸一致");

  hzw::Image empty;
  CHECK(!hzw::encode_jpeg("/tmp/hzw_test_jpeg_empty.jpg", empty, 85),
        "空 Image 编码应失败");
  CHECK(!hzw::decode_jpeg("/tmp/this_file_does_not_exist_xxxyyy.jpg", back),
        "不存在文件 decode 应失败");

  hzw::Image small = make_gradient(640, 360);
  const std::string small_path = "/tmp/hzw_test_jpeg_small.jpg";
  bool enc2 = hzw::encode_jpeg(small_path, small, 50);
  CHECK(enc2 && file_exists(small_path) && file_size(small_path) > 0,
        "640x360 低质量编码成功");

  hzw::Image q = make_gradient(320, 180);
  bool q_hi = hzw::encode_jpeg("/tmp/hzw_test_jpeg_qhi.jpg", q, 100);
  bool q_lo = hzw::encode_jpeg("/tmp/hzw_test_jpeg_qlo.jpg", q, 1);
  CHECK(q_hi && q_lo, "quality=1 / quality=100 编码均成功");

  std::printf("\n%s (%d 失败)\n", failures == 0 ? "PASS" : "FAIL", failures);
  return failures == 0 ? 0 : 1;
}
