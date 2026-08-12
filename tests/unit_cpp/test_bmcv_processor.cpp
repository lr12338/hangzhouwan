// -*- coding: utf-8 -*-
// =============================================================================
// BmcvProcessor 单元测试（阶段4）。
//
// 覆盖：
//   - BMCV 资源重复创建和释放（init/cleanup 多次循环，无泄漏/崩溃）；
//   - 预处理输出尺寸（640x640 NCHW）；
//   - 归一化范围（[0,1]，无 NaN/Inf）；
//   - CPU/BMCV 颜色通道一致性（BMCV CSC RGB 与 sws NV12->RGB 均值差异 < 5）；
//   - 绘制矩形后 NV12 Y 分量变化（验证 draw_rectangle 生效）。
//
// 需要 BM1684 硬件（bm_dev_request）。使用合成 NV12 帧，不依赖视频文件或 bmodel。
// =============================================================================
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "video/bmcv_processor.h"

extern "C" {
#include "bmlib_runtime.h"
#include "bmcv_api_ext.h"
#include <libswscale/swscale.h>
}

namespace {
int failures = 0;
void check(bool cond, const char* msg) {
  if (cond) { printf("  [通过] %s\n", msg); }
  else { printf("  [失败] %s\n", msg); failures++; }
}

// 创建合成 NV12 帧：Y=128 全灰，UV=128（中性色）
struct SynthFrame {
  int w, h;
  std::vector<uint8_t> y, uv;
  int linesize;
  SynthFrame(int w, int h) : w(w), h(h), y(w * h, 128), uv(w * h / 2, 128), linesize(w) {}
};
}  // namespace

int main() {
  setvbuf(stdout, NULL, _IOLBF, 0);
  const int SW = 960, SH = 544;

  bm_handle_t handle;
  bm_status_t st = bm_dev_request(&handle, 0);
  if (st != BM_SUCCESS || !handle) {
    printf("SKIP: 无法打开 BM 设备（ret=%d），跳过 BMCV 测试\n", (int)st);
    return 0;  // 无硬件环境时跳过而非失败
  }
  printf("BMCV 单元测试（设备已打开）\n");

  // === 测试 1：BMCV 资源重复创建和释放 ===
  printf("== 测试 1: 重复创建/释放 ==\n");
  {
    hzw::BmcvProcessor proc;
    std::string err;
    for (int i = 0; i < 5; ++i) {
      check(proc.init(handle, SW, SH, err), "init 循环成功");
      check(proc.ready(), "ready() 为 true");
    }
    // 析构时自动清理
  }
  printf("  5 次 init/析构循环完成，无崩溃\n");

  // === 测试 2-4：预处理输出尺寸、归一化范围、颜色一致性 ===
  printf("== 测试 2-4: 预处理输出尺寸/归一化/颜色一致性 ==\n");
  {
    hzw::BmcvProcessor proc;
    std::string err;
    check(proc.init(handle, SW, SH, err), "init 成功");

    SynthFrame sf(SW, SH);
    // 设置一些非中性像素以便测试
    for (int y = 0; y < SH; ++y)
      for (int x = 0; x < SW; ++x)
        sf.y[y * SW + x] = static_cast<uint8_t>((x + y) % 256);

    // 构造 AVFrame（仅使用 data/linesize 字段）
    AVFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.data[0] = sf.y.data();
    frame.data[1] = sf.uv.data();
    frame.linesize[0] = SW;
    frame.linesize[1] = SW;
    frame.width = SW;
    frame.height = SH;

    std::vector<float> out(3 * 640 * 640, -1.0f);  // 初始化为 -1 检测是否全部写入
    check(proc.preprocess(&frame, out.data(), err), "preprocess 成功");

    // 测试 2：输出尺寸 = 3*640*640（已全部写入，无 -1 残留）
    bool all_written = true;
    for (float v : out) { if (v < -0.5f) { all_written = false; break; } }
    check(all_written, "输出尺寸正确（3*640*640 全部写入）");

    // 测试 3：归一化范围 [0, 1]，无 NaN/Inf
    bool range_ok = true, no_nan = true;
    for (float v : out) {
      if (std::isnan(v) || std::isinf(v)) { no_nan = false; break; }
      if (v < -0.001f || v > 1.001f) { range_ok = false; break; }
    }
    check(no_nan, "无 NaN/Inf");
    check(range_ok, "归一化范围 [0,1]");

    // 测试 4：CPU/BMCV 颜色通道一致性
    // CPU 参考：sws NV12->RGB_PACKED 640x640 -> 归一化
    SwsContext* sws = sws_getContext(SW, SH, AV_PIX_FMT_NV12, 640, 640, AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    std::vector<uint8_t> rgb640(640 * 640 * 3, 0);
    const uint8_t* src[2] = {sf.y.data(), sf.uv.data()};
    const int sstr[2] = {SW, SW};
    uint8_t* dst[1] = {rgb640.data()};
    const int dstr[1] = {640 * 3};
    sws_scale(sws, src, sstr, 0, SH, dst, dstr);
    sws_freeContext(sws);

    // BMCV 预处理 vs CPU 预处理：均值差异应 < 5/255（CSC 系数差异）
    double sum_diff = 0;
    int n = 640 * 640;
    for (int i = 0; i < n; ++i) {
      float cpu_r = rgb640[i * 3] / 255.0f;
      float cpu_g = rgb640[i * 3 + 1] / 255.0f;
      float cpu_b = rgb640[i * 3 + 2] / 255.0f;
      sum_diff += std::fabs(out[i] - cpu_r);
      sum_diff += std::fabs(out[n + i] - cpu_g);
      sum_diff += std::fabs(out[2 * n + i] - cpu_b);
    }
    double mean_diff = sum_diff / (3 * n);
    printf("  BMCV vs CPU 均值差异 = %.4f (阈值 5/255=%.4f)\n", mean_diff, 5.0 / 255.0);
    check(mean_diff < 5.0 / 255.0, "颜色通道一致性（均值差异 < 5/255）");
  }

  // === 测试 5：绘制矩形后 Y 分量变化 ===
  printf("== 测试 5: 绘制矩形 ==\n");
  {
    hzw::BmcvProcessor proc;
    std::string err;
    check(proc.init(handle, SW, SH, err), "init 成功");

    SynthFrame sf(SW, SH);  // 全灰 Y=128
    AVFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.data[0] = sf.y.data();
    frame.data[1] = sf.uv.data();
    frame.linesize[0] = SW;
    frame.linesize[1] = SW;
    frame.width = SW;
    frame.height = SH;

    // 记录绘制前的 Y 值
    uint8_t y_before = sf.y[100 * SW + 150];

    std::vector<hzw::Detection> dets;
    hzw::Detection d;
    d.x1 = 100; d.y1 = 100; d.x2 = 300; d.y2 = 300; d.score = 0.5f;
    dets.push_back(d);

    check(proc.draw_rectangles(&frame, dets, err), "draw_rectangles 成功");

    // 绘制后 Y 分量应变化（矩形边框处）
    uint8_t y_after = sf.y[100 * SW + 150];
    printf("  Y before=%d after=%d at (150,100)\n", y_before, y_after);
    check(y_before != y_after, "绘制后 Y 分量变化（draw_rectangle 生效）");

    // 空检测列表不应崩溃
    std::vector<hzw::Detection> empty;
    check(proc.draw_rectangles(&frame, empty, err), "空检测列表不崩溃");
  }

  // === 测试 6：BMCV NV12 输出缩放 ===
  printf("== 测试 6: NV12 输出缩放 ==\n");
  {
    hzw::BmcvProcessor proc;
    std::string err;
    check(proc.init(handle, SW, SH, err), "init 成功");
    SynthFrame sf(SW, SH);
    AVFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.data[0] = sf.y.data();
    frame.data[1] = sf.uv.data();
    frame.linesize[0] = SW;
    frame.linesize[1] = SW;
    frame.width = SW;
    frame.height = SH;
    AVFrame* scaled = nullptr;
    std::vector<hzw::BmcvProcessor::ColoredRect> overlays;
    hzw::BmcvProcessor::ColoredRect overlay;
    overlay.x1 = 100; overlay.y1 = 100; overlay.x2 = 300; overlay.y2 = 300;
    overlay.r = 0; overlay.g = 255; overlay.b = 0; overlay.label = "ship 0.95";
    overlays.push_back(overlay);
    check(proc.resize_yuv420p(&frame, 1280, 720, &scaled, err, &overlays),
          "resize_yuv420p 1280x720 成功");
    check(scaled != nullptr, "缩放输出 AVFrame 非空");
    if (scaled) {
      check(scaled->width == 1280 && scaled->height == 720,
            "缩放输出尺寸为 1280x720");
      check(scaled->format == AV_PIX_FMT_YUV420P,
            "缩放输出格式为 YUV420P");
      check(scaled->data[0] != nullptr && scaled->data[1] != nullptr &&
                scaled->data[2] != nullptr,
            "缩放输出携带三个主机像素平面");
      check(scaled->data[4] == nullptr && scaled->data[5] == nullptr &&
                scaled->data[6] == nullptr,
            "缩放输出不伪造 DMA 设备平面");
      check(scaled->buf[0] != nullptr,
            "缩放输出具有 FFmpeg 引用计数生命周期");
      check(scaled->linesize[0] >= 1280 && scaled->linesize[1] >= 640 &&
                scaled->linesize[2] >= 640,
            "缩放输出平面步长有效");
      bool luma_has_content = false;
      for (int y = 0; y < scaled->height && !luma_has_content; ++y) {
        for (int x = 0; x < scaled->width; ++x) {
          if (scaled->data[0][y * scaled->linesize[0] + x] != 0) {
            luma_has_content = true;
            break;
          }
        }
      }
      check(luma_has_content, "缩放输出包含真实亮度像素");
      av_frame_free(&scaled);
    }
  }

  // === 测试 6：crop_to_rgb（抓拍裁剪路径，设备侧 crop+CSC）===
  printf("== 测试 6: crop_to_rgb ==\n");
  {
    hzw::BmcvProcessor proc;
    std::string err;
    check(proc.init(handle, SW, SH, err), "init 成功");

    SynthFrame sf(SW, SH);
    // 设置可识别的亮度渐变
    for (int y = 0; y < SH; ++y)
      for (int x = 0; x < SW; ++x)
        sf.y[y * SW + x] = static_cast<uint8_t>((x + y) % 256);

    AVFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.data[0] = sf.y.data();
    frame.data[1] = sf.uv.data();
    frame.linesize[0] = SW;
    frame.linesize[1] = SW;
    frame.width = SW;
    frame.height = SH;

    // crop 中心 200x150 区域
    const int CX = 300, CY = 200, CW = 200, CH = 150;
    hzw::Image crop;
    check(proc.crop_to_rgb(&frame, CX, CY, CW, CH, crop, err), "crop_to_rgb 成功");
    check(crop.valid(), "crop Image 有效");
    check(crop.width == CW, "crop 宽度正确");
    check(crop.height == CH, "crop 高度正确");
    check((int)crop.data.size() == CW * CH * 3, "crop 数据大小正确");
    // 内容非空
    bool has_content = false;
    for (size_t i = 0; i < crop.data.size(); ++i) {
      if (crop.data[i] != 0) { has_content = true; break; }
    }
    check(has_content, "crop 包含真实像素");

    // 边界 clamp：crop 越界区域应自动收缩
    hzw::Image crop2;
    check(proc.crop_to_rgb(&frame, SW - 60, SH - 60, 100, 100, crop2, err),
          "crop 越界自动 clamp 成功");
    check(crop2.width <= 60 && crop2.height <= 60, "crop 越界后尺寸收缩");

    // 重复 crop 不同尺寸（验证 crop_rgb 按需重建无泄漏）
    for (int i = 0; i < 3; ++i) {
      hzw::Image c;
      int w = 50 + i * 30, h = 40 + i * 20;
      check(proc.crop_to_rgb(&frame, 10, 10, w, h, c, err), "重复 crop 不同尺寸成功");
    }
  }
  printf("  crop_to_rgb 测试完成\n");

  bm_dev_free(handle);
  printf("\n===== BMCV 测试: %d 失败 =====\n", failures);
  return failures > 0 ? 1 : 0;
}
