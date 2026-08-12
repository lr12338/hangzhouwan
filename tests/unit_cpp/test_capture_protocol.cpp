// -*- coding: utf-8 -*-
// Bridge Capture 协议帧编解码 + JSON 提取测试。无硬件依赖。
#include <cassert>
#include <cstdio>
#include <string>

#include "capture/capture_protocol.h"

static int failures = 0;
#define CHECK(cond) do { if(!(cond)){ std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while(0)

void test_frame_roundtrip() {
  std::string json = R"({"cmd":"arm","bridge":"north"})";
  std::string frame = hzw::encode_frame(json);
  CHECK(frame.size() == 4 + json.size());
  // 长度前缀大端
  CHECK((uint8_t)frame[0] == 0 && (uint8_t)frame[1] == 0 &&
        (uint8_t)frame[2] == 0 && (uint8_t)frame[3] == (uint8_t)json.size());
  std::string out;
  size_t consumed = 0;
  bool error = false;
  CHECK(hzw::decode_frame(frame, out, consumed, error));
  CHECK(!error);
  CHECK(out == json);
  CHECK(consumed == frame.size());
}

void test_frame_incomplete() {
  std::string json = R"({"cmd":"status"})";
  std::string frame = hzw::encode_frame(json);
  // 截断
  std::string partial = frame.substr(0, 6);
  std::string out;
  size_t consumed = 0;
  bool error = false;
  CHECK(!hzw::decode_frame(partial, out, consumed, error));
  CHECK(!error);
  CHECK(consumed == 0);
}

void test_frame_bad_length() {
  char bad[4] = {static_cast<char>(0xFF), static_cast<char>(0xFF), static_cast<char>(0xFF), static_cast<char>(0xFF)};
  std::string buf(bad, 4);
  std::string out;
  size_t consumed = 0;
  bool error = false;
  CHECK(!hzw::decode_frame(buf, out, consumed, error));
  CHECK(error);
}

void test_json_extract() {
  std::string j = R"({"cmd":"arm","session_id":"sess-1","bridge":"north","mmsi":"414402810","direction":"upstream","trigger_ts_ms":1786506953000,"distance_to_gate_m":458.5,"eta_sec":89.0,"timeout_sec":180,"accepted":true})";
  CHECK(hzw::json_get_string(j, "cmd") == "arm");
  CHECK(hzw::json_get_string(j, "session_id") == "sess-1");
  CHECK(hzw::json_get_string(j, "bridge") == "north");
  CHECK(hzw::json_get_string(j, "mmsi") == "414402810");
  CHECK(hzw::json_get_int(j, "trigger_ts_ms") == 1786506953000LL);
  CHECK(hzw::json_get_int(j, "timeout_sec") == 180);
  CHECK(std::abs(hzw::json_get_double(j, "distance_to_gate_m") - 458.5) < 0.01);
  CHECK(std::abs(hzw::json_get_double(j, "eta_sec") - 89.0) < 0.01);
  CHECK(hzw::json_get_bool(j, "accepted") == true);
  // 缺省值
  CHECK(hzw::json_get_int(j, "nonexistent", 42) == 42);
  CHECK(hzw::json_get_string(j, "nonexistent") == "");
}

void test_make_arm_json() {
  std::string j = hzw::make_arm_json("s1", "north", "414402810", "upstream",
                                     1786506953000LL, 458.5, 89.0, 180);
  CHECK(hzw::json_get_string(j, "cmd") == "arm");
  CHECK(hzw::json_get_string(j, "bridge") == "north");
  CHECK(hzw::json_get_int(j, "trigger_ts_ms") == 1786506953000LL);
  CHECK(std::abs(hzw::json_get_double(j, "eta_sec") - 89.0) < 0.01);
}

void test_make_responses() {
  std::string arm = hzw::make_arm_response(true, "s1", "QUEUED");
  CHECK(hzw::json_get_bool(arm, "accepted") == true);
  CHECK(hzw::json_get_string(arm, "state") == "QUEUED");
  std::string arm2 = hzw::make_arm_response(false, "s1", "FAILED", "bridge busy");
  CHECK(hzw::json_get_bool(arm2, "accepted") == false);
  CHECK(hzw::json_get_string(arm2, "reason") == "bridge busy");
  std::string health = hzw::make_health_json(true, 123, 0, 5, "/w/y.bmodel");
  CHECK(hzw::json_get_bool(health, "healthy") == true);
  CHECK(hzw::json_get_int(health, "captured_total") == 5);
  CHECK(hzw::json_get_string(health, "bmodel") == "/w/y.bmodel");
}

int main() {
  test_frame_roundtrip();
  test_frame_incomplete();
  test_frame_bad_length();
  test_json_extract();
  test_make_arm_json();
  test_make_responses();
  if (failures == 0) { std::printf("capture_protocol: all passed\n"); return 0; }
  std::printf("capture_protocol: %d failures\n", failures);
  return 1;
}
