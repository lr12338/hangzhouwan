// -*- coding: utf-8 -*-
// 极简 SHA-256（仅用于在 JSON 中如实填写 model_sha256，避免硬编码）。
#ifndef HZW_UTIL_SHA256_H
#define HZW_UTIL_SHA256_H
#include <cstdint>
#include <cstddef>
#include <string>
namespace hzw {
// 返回小写十六进制 SHA-256。
std::string sha256_hex(const uint8_t* data, size_t len);
std::string sha256_hex(const std::string& data);
// 分块读取文件并计算 SHA-256。文件不存在或读失败返回空串。
std::string sha256_file(const std::string& path);
}
#endif
