// -*- coding: utf-8 -*-
// =============================================================================
// 阶段3 单图 C++ 推理 PoC：
//   JPEG -> libjpeg 解码(RGB) -> 直接 resize 640x640 -> /255 -> NCHW FLOAT32
//   -> BMRuntime 推理 -> [1,25200,6] -> YOLOv7 置信度过滤/坐标换算/NMS -> 绘框 -> JPEG+JSON
//
// 用法：
//   ./single_image_infer --bmodel <bmodel> --image <jpg> \
//       --output <result.jpg> --json <result.json> \
//       --conf 0.1 --iou 0.1 --device 0 [--loop N]
// =============================================================================
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "image_io/jpeg_io.h"
#include "inference/bmrt_detector.h"
#include "inference/yolov7_postprocess.h"
#include "util/sha256.h"

namespace {

using Clock = std::chrono::steady_clock;
double ms_between(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

// 递归创建父目录（POSIX）。
bool ensure_parent_dir(const std::string& path) {
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return true;
  std::string dir = path.substr(0, slash);
  if (dir.empty()) return true;
  std::string acc;
  for (size_t i = 0; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == '/') {
      if (!acc.empty()) {
        mkdir(acc.c_str(), 0775);  // 忽略已存在
      }
    }
    if (i < dir.size()) acc += dir[i];
  }
  return true;
}

// 预处理：RGB Image -> NCHW FLOAT32 [1,3,640,640]（与原 Python 契约一致）。
//   BGR->RGB：libjpeg 直接解码为 RGB，等价于 cv2 BGR->RGB。
//   直接 resize（无 letterbox）/ 除以 255 / HWC->CHW / 加 batch 维。
bool preprocess(const hzw::Image& rgb, int input_w, int input_h,
                std::vector<float>& out) {
  if (!rgb.valid()) return false;
  hzw::Image resized;
  if (!hzw::resize_bilinear(rgb, input_w, input_h, resized)) return false;
  const int C = hzw::Image::channels;
  out.assign(static_cast<size_t>(1) * C * input_w * input_h, 0.0f);
  // HWC -> CHW，除以 255
  for (int c = 0; c < C; ++c) {
    float* plane = out.data() + static_cast<size_t>(c) * input_w * input_h;
    for (int y = 0; y < input_h; ++y) {
      for (int x = 0; x < input_w; ++x) {
        const uint8_t* px = resized.pixel(x, y);
        plane[y * input_w + x] = static_cast<float>(px[c]) / 255.0f;
      }
    }
  }
  return true;
}

// JSON 字符串转义。
std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o += c;
        }
    }
  }
  return o;
}

std::string shape_to_json(const std::vector<int>& s) {
  std::ostringstream os;
  os << "[";
  for (size_t i = 0; i < s.size(); ++i) {
    if (i) os << ", ";
    os << s[i];
  }
  os << "]";
  return os.str();
}

bool write_json(const std::string& path, const std::string& model_sha,
                const std::string& image_path, int ow, int oh,
                const std::vector<int>& in_shape,
                const std::vector<int>& out_shape,
                double pre_ms, double inf_ms, double post_ms,
                const std::vector<hzw::Detection>& dets) {
  ensure_parent_dir(path);
  FILE* fp = std::fopen(path.c_str(), "w");
  if (!fp) return false;
  std::ostringstream os;
  os << std::fixed << std::setprecision(6);
  os << "{\n";
  os << "  \"model_sha256\": \"" << model_sha << "\",\n";
  os << "  \"input_image\": \"" << json_escape(image_path) << "\",\n";
  os << "  \"original_width\": " << ow << ",\n";
  os << "  \"original_height\": " << oh << ",\n";
  os << "  \"input_shape\": " << shape_to_json(in_shape) << ",\n";
  os << "  \"output_shape\": " << shape_to_json(out_shape) << ",\n";
  os << "  \"preprocess_ms\": " << pre_ms << ",\n";
  os << "  \"inference_ms\": " << inf_ms << ",\n";
  os << "  \"postprocess_ms\": " << post_ms << ",\n";
  os << "  \"detections\": [\n";
  for (size_t i = 0; i < dets.size(); ++i) {
    const auto& d = dets[i];
    os << "    {\n";
    os << "      \"class_id\": " << d.class_id << ",\n";
    os << "      \"class_name\": \"" << json_escape(d.class_name) << "\",\n";
    os << "      \"score\": " << d.score << ",\n";
    os << "      \"x1\": " << d.x1 << ",\n";
    os << "      \"y1\": " << d.y1 << ",\n";
    os << "      \"x2\": " << d.x2 << ",\n";
    os << "      \"y2\": " << d.y2 << "\n";
    os << "    }" << (i + 1 < dets.size() ? "," : "") << "\n";
  }
  os << "  ]\n";
  os << "}\n";
  std::string s = os.str();
  bool ok = std::fwrite(s.data(), 1, s.size(), fp) == s.size();
  std::fclose(fp);
  return ok;
}

struct Args {
  std::string bmodel;
  std::string image;
  std::string output = "result.jpg";
  std::string json = "result.json";
  float conf = 0.1f;
  float iou = 0.1f;
  int device = 0;
  int loop = 1;
};

bool parse_args(int argc, char** argv, Args& a, std::string& err) {
  auto need = [&](int& i, const char* name, std::string& dst) -> bool {
    if (i + 1 >= argc) { err = std::string("缺少参数: ") + name; return false; }
    dst = argv[++i]; return true;
  };
  auto needf = [&](int& i, const char* name, float& dst) -> bool {
    if (i + 1 >= argc) { err = std::string("缺少参数: ") + name; return false; }
    dst = std::stof(std::string(argv[++i])); return true;
  };
  auto needi = [&](int& i, const char* name, int& dst) -> bool {
    if (i + 1 >= argc) { err = std::string("缺少参数: ") + name; return false; }
    dst = std::stoi(std::string(argv[++i])); return true;
  };
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    if (k == "--bmodel") { if (!need(i, "--bmodel", a.bmodel)) return false; }
    else if (k == "--image") { if (!need(i, "--image", a.image)) return false; }
    else if (k == "--output") { if (!need(i, "--output", a.output)) return false; }
    else if (k == "--json") { if (!need(i, "--json", a.json)) return false; }
    else if (k == "--conf") { if (!needf(i, "--conf", a.conf)) return false; }
    else if (k == "--iou") { if (!needf(i, "--iou", a.iou)) return false; }
    else if (k == "--device") { if (!needi(i, "--device", a.device)) return false; }
    else if (k == "--loop") { if (!needi(i, "--loop", a.loop)) return false; }
    else if (k == "--help" || k == "-h") {
      err = "help"; return false;
    } else {
      err = std::string("未知参数: ") + k; return false;
    }
  }
  if (a.bmodel.empty()) { err = "缺少 --bmodel"; return false; }
  if (a.image.empty()) { err = "缺少 --image"; return false; }
  return true;
}

void print_help() {
  std::cerr <<
    "用法: single_image_infer --bmodel <bmodel> --image <jpg> "
    "--output <result.jpg> --json <result.json> --conf 0.1 --iou 0.1 --device 0 [--loop N]\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  std::string err;
  if (!parse_args(argc, argv, args, err)) {
    if (err == "help") { print_help(); return 0; }
    std::cerr << "错误 | 参数 | " << err << "\n";
    print_help();
    return 2;
  }
  if (args.loop < 1) args.loop = 1;

  // 1) 加载模型（仅一次）
  hzw::BmrtDetector det(args.device, args.bmodel);
  if (!det.ok()) {
    std::cerr << "错误 | 模型加载 | " << det.last_error() << "\n";
    return 3;
  }
  std::cout << "信息 | 模型 | 网络=" << det.net_name()
            << " 输入=" << det.input_name() << " 输出=" << det.output_name() << "\n";

  // 输出目录自动创建（测试用例 7）
  ensure_parent_dir(args.output);
  ensure_parent_dir(args.json);

  // 2) 解码图片（RGB）
  hzw::Image img;
  if (!hzw::decode_jpeg(args.image, img)) {
    std::cerr << "错误 | 图片解码 | " << args.image << "\n";
    return 4;
  }
  const int ow = img.width;
  const int oh = img.height;

  // 3) 预处理
  auto t0 = Clock::now();
  std::vector<float> input;
  const int input_size = 640;
  if (!preprocess(img, input_size, input_size, input)) {
    std::cerr << "错误 | 预处理失败\n";
    return 5;
  }
  auto t1 = Clock::now();

  // 4) 推理（首帧计时；--loop N 重复推理不重新加载模型）
  std::vector<float> output;
  if (!det.infer(input, output)) {
    std::cerr << "错误 | 推理 | " << det.last_error() << "\n";
    return 6;
  }
  auto t2 = Clock::now();

  // 5) 后处理
  const int num_boxes = det.output_shape().size() >= 2 ? det.output_shape()[1] : 25200;
  const int num_vals = det.output_shape().size() >= 3 ? det.output_shape()[2] : 6;
  std::vector<hzw::Detection> dets;
  hzw::postprocess_yolov7(output.data(), num_boxes, num_vals, ow, oh,
                          input_size, args.conf, args.iou, dets);
  auto t3 = Clock::now();

  const double pre_ms = ms_between(t0, t1);
  const double inf_ms = ms_between(t1, t2);
  const double post_ms = ms_between(t2, t3);

  std::cout << "信息 | 计时 | 预处理=" << pre_ms << "ms 推理=" << inf_ms
            << "ms 后处理=" << post_ms << "ms\n";
  std::cout << "信息 | 检测 | 数量=" << dets.size()
            << " (conf=" << args.conf << " iou=" << args.iou << ")\n";
  for (size_t i = 0; i < dets.size() && i < 20; ++i) {
    const auto& d = dets[i];
    std::cout << "  [" << i << "] " << d.class_name << " score=" << d.score
              << " box=(" << d.x1 << "," << d.y1 << "," << d.x2 << "," << d.y2 << ")\n";
  }

  // 6) 绘框 + 输出 JPEG
  {
    hzw::Image out_img = img;  // 在原图上绘制
    for (const auto& d : dets) {
      hzw::Color c{0, 255, 0};  // 绿色框（RGB）
      hzw::draw_rectangle(out_img, static_cast<int>(d.x1), static_cast<int>(d.y1),
                          static_cast<int>(d.x2), static_cast<int>(d.y2), c, 2);
      std::ostringstream lab;
      lab << d.class_name << " " << std::fixed << std::setprecision(2) << d.score;
      hzw::draw_label(out_img, static_cast<int>(d.x1), static_cast<int>(d.y1),
                      lab.str(), c, 2);
    }
    if (!hzw::encode_jpeg(args.output, out_img, 92)) {
      std::cerr << "错误 | 输出图片写入失败: " << args.output << "\n";
      return 7;
    }
    std::cout << "信息 | 输出图片 | " << args.output << "\n";
  }

  // 7) JSON
  {
    std::string model_sha = hzw::sha256_file(args.bmodel);
    if (model_sha.empty()) {
      std::cerr << "警告 | model_sha256 计算失败（文件不可读）\n";
    }
    if (!write_json(args.json, model_sha, args.image, ow, oh,
                    det.input_shape(), det.output_shape(),
                    pre_ms, inf_ms, post_ms, dets)) {
      std::cerr << "错误 | JSON 写入失败: " << args.json << "\n";
      return 8;
    }
    std::cout << "信息 | 输出JSON | " << args.json << "\n";
  }

  // 8) 重复推理（阶段3 13.2）：同一模型实例连续推理 N 次，统计耗时与框数稳定性
  if (args.loop > 1) {
    std::vector<double> times;
    times.reserve(args.loop);
    int box_count = -1;
    bool stable = true;
    for (int i = 0; i < args.loop; ++i) {
      std::vector<float> o;
      auto a = Clock::now();
      if (!det.infer(input, o)) {
        std::cerr << "错误 | 重复推理第 " << i << " 次失败: " << det.last_error() << "\n";
        return 9;
      }
      auto b = Clock::now();
      times.push_back(ms_between(a, b));
      std::vector<hzw::Detection> dd;
      hzw::postprocess_yolov7(o.data(), num_boxes, num_vals, ow, oh,
                              input_size, args.conf, args.iou, dd);
      if (box_count < 0) box_count = static_cast<int>(dd.size());
      else if (box_count != static_cast<int>(dd.size())) stable = false;
    }
    std::sort(times.begin(), times.end());
    double sum = 0;
    for (double t : times) sum += t;
    double avg = sum / times.size();
    double p50 = times[times.size() / 2];
    double p95 = times[static_cast<size_t>(times.size() * 0.95)];
    if (p95 < times.back()) p95 = times.back();
    double mx = times.back();
    std::cout << "信息 | 重复推理 | 次数=" << args.loop
              << " 平均=" << avg << "ms P50=" << p50 << "ms P95=" << p95
              << "ms 最大=" << mx << "ms 框数=" << box_count
              << " 稳定=" << (stable ? "是" : "否") << "\n";
  }

  return 0;
}
