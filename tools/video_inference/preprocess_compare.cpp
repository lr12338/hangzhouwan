// -*- coding: utf-8 -*-
// =============================================================================
// CPU/BMCV 预处理正确性对照工具（阶段4 验收用）。
//
// 解码 test.mp4 指定帧，分别用 CPU 和 BMCV 预处理 -> 推理 -> 后处理，
// 比较检测数量、score、框坐标 IoU、NaN/Inf。
//
// 用法：./preprocess_compare [--bmodel <path>] [--input <mp4>] [--frames 30,50,100,...]
// 默认帧：30 + 3 有检测 + 1 无检测 + 3 随机。
// =============================================================================
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "inference/bmrt_detector.h"
#include "inference/yolov7_postprocess.h"
#include "video/bmcv_processor.h"
#include "video/ffmpeg_compat.h"
#include "video/video_source.h"

namespace hzw {

namespace {
// CPU 预处理：NV12 -> RGB640 (sws) -> NCHW FLOAT32 /255
struct CpuPre {
  SwsContext* sws = nullptr;
  std::vector<uint8_t> rgb640;
  std::vector<float> out;
  void init(int sw, int sh) {
    sws = sws_getContext(sw, sh, AV_PIX_FMT_NV12, 640, 640, AV_PIX_FMT_RGB24,
                         SWS_BILINEAR, nullptr, nullptr, nullptr);
    rgb640.assign(640 * 640 * 3, 0);
    out.assign(3 * 640 * 640, 0.0f);
  }
  ~CpuPre() { if (sws) sws_freeContext(sws); }
  bool run(AVFrame* f, int sw, int sh) {
    const uint8_t* src[2] = {f->data[0], f->data[1]};
    const int sstr[2] = {f->linesize[0], f->linesize[1]};
    uint8_t* dst[1] = {rgb640.data()};
    const int dstr[1] = {640 * 3};
    sws_scale(sws, src, sstr, 0, sh, dst, dstr);
    const float inv = 1.0f / 255.0f;
    const int plane = 640 * 640;
    const uint8_t* p = rgb640.data();
    for (int i = 0; i < plane; ++i) {
      out[i] = p[0] * inv;
      out[plane + i] = p[1] * inv;
      out[2 * plane + i] = p[2] * inv;
      p += 3;
    }
    return true;
  }
};

float iou(const Detection& a, const Detection& b) {
  float xx1 = std::max(a.x1, b.x1), yy1 = std::max(a.y1, b.y1);
  float xx2 = std::min(a.x2, b.x2), yy2 = std::min(a.y2, b.y2);
  float w = std::max(0.0f, xx2 - xx1), h = std::max(0.0f, yy2 - yy1);
  float inter = w * h;
  float ua = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
  return ua > 0 ? inter / ua : 0.0f;
}

bool has_nan_inf(const std::vector<float>& v) {
  for (float x : v) { if (std::isnan(x) || std::isinf(x)) return true; }
  return false;
}

void compare(int seq, const std::vector<Detection>& cpu,
             const std::vector<Detection>& bmcv, int& pass, int& fail) {
  printf("帧 %d: CPU检测=%zu BMCV检测=%zu", seq, cpu.size(), bmcv.size());
  bool ok = true;
  if (cpu.size() != bmcv.size()) {
    printf(" [数量不一致]");
    ok = false;
  }
  float min_iou = 1.0f, max_score_diff = 0.0f;
  // 贪心匹配：每个 CPU 框找 IoU 最高的 BMCV 框
  std::vector<bool> used(bmcv.size(), false);
  for (const auto& c : cpu) {
    float best = 0; int bi = -1;
    for (size_t j = 0; j < bmcv.size(); ++j) {
      if (used[j]) continue;
      float v = iou(c, bmcv[j]);
      if (v > best) { best = v; bi = (int)j; }
    }
    if (bi >= 0) {
      used[bi] = true;
      if (best < min_iou) min_iou = best;
      float sd = std::fabs(c.score - bmcv[bi].score);
      if (sd > max_score_diff) max_score_diff = sd;
      if (best < 0.98f) { printf(" [IoU=%.3f<0.98]", best); ok = false; }
      if (sd > 0.01f) { printf(" [score差=%.4f>0.01]", sd); ok = false; }
    } else {
      printf(" [CPU框无匹配]"); ok = false;
    }
  }
  if (cpu.empty() && bmcv.empty()) min_iou = 1.0f;
  printf(" min_IoU=%.3f max_score_diff=%.4f", min_iou, max_score_diff);
  if (ok) { printf(" -> 通过\n"); pass++; } else { printf(" -> 不一致\n"); fail++; }
}
}  // namespace
}  // namespace hzw

int main(int argc, char** argv) {
  std::string input = "testdata/test.mp4";
  std::string bmodel = "artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel";
  std::string frames_str = "30,40,50,80,100,120,150,180";
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    if (k == "--input" && i + 1 < argc) input = argv[++i];
    else if (k == "--bmodel" && i + 1 < argc) bmodel = argv[++i];
    else if (k == "--frames" && i + 1 < argc) frames_str = argv[++i];
  }

  // parse frames
  std::vector<int> frames;
  { std::string s; for (char c : frames_str) { if (c == ',') { frames.push_back(std::atoi(s.c_str())); s.clear(); } else s += c; } if (!s.empty()) frames.push_back(std::atoi(s.c_str())); }
  int max_frame = 0; for (int f : frames) if (f > max_frame) max_frame = f;

  hzw::SophonVideoSource src;
  std::string err;
  if (!src.open(input, 0, 20, "h264_bm", err)) {
    fprintf(stderr, "打开视频失败: %s\n", err.c_str());
    return 1;
  }
  int sw = src.width(), sh = src.height();
  printf("视频: %dx%d 对比帧: ", sw, sh);
  for (int f : frames) printf("%d ", f);
  printf("\n");

  hzw::BmrtDetector det(0, bmodel);
  if (!det.ok()) { fprintf(stderr, "模型加载失败: %s\n", det.last_error().c_str()); return 2; }

  hzw::BmcvProcessor bmcv;
  if (!bmcv.init(det.handle(), sw, sh, err)) {
    fprintf(stderr, "BMCV 初始化失败: %s\n", err.c_str());
    return 3;
  }

  hzw::CpuPre cpu_pre;
  cpu_pre.init(sw, sh);

  const int num_boxes = det.output_shape().size() >= 2 ? det.output_shape()[1] : 25200;
  const int num_vals = det.output_shape().size() >= 3 ? det.output_shape()[2] : 6;

  int pass = 0, fail = 0;
  hzw::VideoFrame vf;
  int frame_idx = 0;
  while (!frames.empty() && src.read(vf, err)) {
    if (frame_idx == frames.front()) {
      frames.erase(frames.begin());
      AVFrame* f = vf.frame;
      // CPU preprocess
      std::vector<float> out;
      cpu_pre.run(f, sw, sh);
      std::vector<float> cpu_out;
      if (!det.infer(cpu_pre.out, cpu_out)) {
        fprintf(stderr, "帧 %d CPU 推理失败\n", frame_idx);
        vf.release();
        frame_idx++;
        continue;
      }
      if (hzw::has_nan_inf(cpu_out)) { printf("帧 %d: CPU 输出含 NaN/Inf\n", frame_idx); fail++; }
      std::vector<hzw::Detection> cpu_dets;
      hzw::postprocess_yolov7(cpu_out.data(), num_boxes, num_vals, sw, sh, 640, 0.1f, 0.1f, cpu_dets);

      // BMCV preprocess
      std::vector<float> bmcv_in(3 * 640 * 640, 0.0f);
      std::vector<float> bmcv_out;
      if (!bmcv.preprocess(f, bmcv_in.data(), err)) {
        fprintf(stderr, "帧 %d BMCV 预处理失败: %s\n", frame_idx, err.c_str());
        vf.release(); frame_idx++; continue;
      }
      if (!det.infer(bmcv_in, bmcv_out)) {
        fprintf(stderr, "帧 %d BMCV 推理失败\n", frame_idx);
        vf.release(); frame_idx++; continue;
      }
      if (hzw::has_nan_inf(bmcv_out)) { printf("帧 %d: BMCV 输出含 NaN/Inf\n", frame_idx); fail++; }
      std::vector<hzw::Detection> bmcv_dets;
      hzw::postprocess_yolov7(bmcv_out.data(), num_boxes, num_vals, sw, sh, 640, 0.1f, 0.1f, bmcv_dets);

      hzw::compare(frame_idx, cpu_dets, bmcv_dets, pass, fail);
    }
    vf.release();
    frame_idx++;
  }

  printf("\n===== 对照汇总: 通过=%d 不一致=%d =====\n", pass, fail);
  return fail > 0 ? 1 : 0;
}
