// -*- coding: utf-8 -*-
#include "video/bmcv_processor.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "bmlib_runtime.h"
#include "bmcv_api_ext.h"
}

namespace hzw {

struct BmcvProcessor::Impl {
  bm_handle_t handle = nullptr;
  int src_w = 0;
  int src_h = 0;

  bm_image src_img{};   // NV12 (DDR1)，CSC 输入
  bm_image csc_img{};   // RGB_PACKED (DDR0)，CSC 输出
  bm_image draw_img{};  // NV12 (DDR0)，绘制用
  bool images_valid = false;

  SwsContext* sws_resize = nullptr;  // RGB_PACKED src -> 640x640
  std::vector<uint8_t> rgb_src;      // RGB_PACKED src_w x src_h (host)
  std::vector<uint8_t> rgb_640;      // RGB_PACKED 640x640 (host)

  bool ready = false;
};

BmcvProcessor::BmcvProcessor() : p_(new Impl()) {}
BmcvProcessor::~BmcvProcessor() {
  if (p_) {
    if (p_->images_valid) {
      bm_image_destroy(p_->src_img);
      bm_image_destroy(p_->csc_img);
      bm_image_destroy(p_->draw_img);
    }
    if (p_->sws_resize) sws_freeContext(p_->sws_resize);
    delete p_;
    p_ = nullptr;
  }
}

bool BmcvProcessor::init(void* handle_void, int src_w, int src_h, std::string& err) {
  if (p_->images_valid) {
    bm_image_destroy(p_->src_img);
    bm_image_destroy(p_->csc_img);
    bm_image_destroy(p_->draw_img);
    p_->images_valid = false;
  }
  if (p_->sws_resize) { sws_freeContext(p_->sws_resize); p_->sws_resize = nullptr; }

  p_->handle = static_cast<bm_handle_t>(handle_void);
  p_->src_w = src_w;
  p_->src_h = src_h;
  if (!p_->handle) { err = "BMCV: 设备句柄为空"; return false; }
  if (src_w <= 0 || src_h <= 0) { err = "BMCV: 源尺寸非法"; return false; }

  bm_handle_t h = p_->handle;
  bm_status_t st;

  // 源 NV12 bm_image（DDR1，CSC/VPP 输入要求 DDR1）。stride 取源宽（960，已对齐）。
  int nv12_stride = src_w;
  int src_stride[2] = {nv12_stride, nv12_stride};
  st = bm_image_create(h, src_h, src_w, FORMAT_NV12, DATA_TYPE_EXT_1N_BYTE,
                       &p_->src_img, src_stride);
  if (st != BM_SUCCESS) { err = "BMCV: 创建源 NV12 失败"; return false; }
  st = bm_image_alloc_dev_mem(p_->src_img, BMCV_HEAP1_ID);
  if (st != BM_SUCCESS) { bm_image_destroy(p_->src_img); err = "BMCV: 分配源设备内存失败"; return false; }

  // CSC 输出：RGB_PACKED src_w x src_h（DDR0）。stride 必须显式指定。
  int csc_stride[1] = {src_w * 3};
  st = bm_image_create(h, src_h, src_w, FORMAT_RGB_PACKED, DATA_TYPE_EXT_1N_BYTE,
                       &p_->csc_img, csc_stride);
  if (st != BM_SUCCESS) { bm_image_destroy(p_->src_img); err = "BMCV: 创建 CSC 输出失败"; return false; }
  st = bm_image_alloc_dev_mem(p_->csc_img, BMCV_HEAP0_ID);
  if (st != BM_SUCCESS) { bm_image_destroy(p_->src_img); bm_image_destroy(p_->csc_img); err = "BMCV: 分配 CSC 设备内存失败"; return false; }

  // sws resize：RGB_PACKED src_w x src_h -> 640x640
  p_->sws_resize = sws_getContext(src_w, src_h, AV_PIX_FMT_RGB24,
                                  640, 640, AV_PIX_FMT_RGB24,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!p_->sws_resize) {
    bm_image_destroy(p_->src_img); bm_image_destroy(p_->csc_img);
    err = "BMCV: 创建 sws resize 失败"; return false;
  }
  p_->rgb_src.assign(static_cast<size_t>(src_w) * src_h * 3, 0);
  p_->rgb_640.assign(static_cast<size_t>(640) * 640 * 3, 0);

  // 绘制：NV12 bm_image（DDR0）
  int draw_stride[2] = {nv12_stride, nv12_stride};
  st = bm_image_create(h, src_h, src_w, FORMAT_NV12, DATA_TYPE_EXT_1N_BYTE,
                       &p_->draw_img, draw_stride);
  if (st != BM_SUCCESS) {
    bm_image_destroy(p_->src_img); bm_image_destroy(p_->csc_img);
    sws_freeContext(p_->sws_resize); p_->sws_resize = nullptr;
    err = "BMCV: 创建绘制 NV12 失败"; return false;
  }
  st = bm_image_alloc_dev_mem(p_->draw_img, BMCV_HEAP0_ID);
  if (st != BM_SUCCESS) {
    bm_image_destroy(p_->src_img); bm_image_destroy(p_->csc_img);
    bm_image_destroy(p_->draw_img);
    sws_freeContext(p_->sws_resize); p_->sws_resize = nullptr;
    err = "BMCV: 分配绘制设备内存失败"; return false;
  }

  p_->images_valid = true;
  p_->ready = true;
  return true;
}

bool BmcvProcessor::ready() const { return p_ && p_->ready; }

bool BmcvProcessor::preprocess(AVFrame* frame, float* out, std::string& err) {
  if (!p_ || !p_->ready) { err = "BMCV: 未初始化"; return false; }
  bm_handle_t h = p_->handle;
  bm_status_t st;

  // 1) H2D: AVFrame NV12 -> 源 bm_image
  void* host_planes[2] = {frame->data[0], frame->data[1]};
  st = bm_image_copy_host_to_device(p_->src_img, host_planes);
  if (st != BM_SUCCESS) { err = "BMCV preprocess: H2D 失败"; return false; }

  // 2) BMCV CSC: NV12 -> RGB_PACKED（storage_convert，约 0.3ms）
  st = bmcv_image_storage_convert(h, 1, &p_->src_img, &p_->csc_img);
  if (st != BM_SUCCESS) { err = "BMCV preprocess: CSC 失败"; return false; }

  // 3) D2H: RGB_PACKED -> host
  void* csc_host[1] = {p_->rgb_src.data()};
  st = bm_image_copy_device_to_host(p_->csc_img, csc_host);
  if (st != BM_SUCCESS) { err = "BMCV preprocess: D2H 失败"; return false; }

  // 4) sws resize: RGB_PACKED src_w x src_h -> 640x640
  const uint8_t* sws_src[1] = {p_->rgb_src.data()};
  const int sws_src_stride[1] = {p_->src_w * 3};
  uint8_t* sws_dst[1] = {p_->rgb_640.data()};
  const int sws_dst_stride[1] = {640 * 3};
  sws_scale(p_->sws_resize, sws_src, sws_src_stride, 0, p_->src_h, sws_dst, sws_dst_stride);

  // 5) CPU 归一化：RGB_PACKED 640x640 -> FLOAT32 NCHW [1,3,640,640]，除以 255
  //    单次遍历直接指针访问（约 7ms，优于原三次遍历 22ms 和 BMCV convert_to）。
  const float inv = 1.0f / 255.0f;
  const int plane = 640 * 640;
  float* rp = out;
  float* gp = out + plane;
  float* bp = out + 2 * plane;
  const uint8_t* p = p_->rgb_640.data();
  for (int i = 0; i < plane; ++i) {
    rp[i] = static_cast<float>(p[0]) * inv;
    gp[i] = static_cast<float>(p[1]) * inv;
    bp[i] = static_cast<float>(p[2]) * inv;
    p += 3;
  }
  return true;
}

bool BmcvProcessor::draw_rectangles(AVFrame* frame, const std::vector<Detection>& dets,
                                     std::string& err) {
  if (!p_ || !p_->ready) { err = "BMCV: 未初始化"; return false; }
  if (dets.empty()) return true;  // 无检测则不绘制，避免无谓 H2D/D2H

  bm_handle_t h = p_->handle;
  bm_status_t st;

  // 1) H2D: AVFrame NV12 -> 绘制 bm_image
  void* host_planes[2] = {frame->data[0], frame->data[1]};
  st = bm_image_copy_host_to_device(p_->draw_img, host_planes);
  if (st != BM_SUCCESS) { err = "BMCV draw: H2D 失败"; return false; }

  // 2) BMCV draw_rectangle（线宽 3，绿色，与 CPU 路径一致）
  //    bmcv_rect_t: {start_x, start_y, crop_w, crop_h}
  for (const auto& d : dets) {
    int x1 = static_cast<int>(d.x1);
    int y1 = static_cast<int>(d.y1);
    int x2 = static_cast<int>(d.x2);
    int y2 = static_cast<int>(d.y2);
    if (x2 <= x1 || y2 <= y1) continue;
    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 > p_->src_w) x2 = p_->src_w; if (y2 > p_->src_h) y2 = p_->src_h;
    bmcv_rect_t rect;
    rect.start_x = x1;
    rect.start_y = y1;
    rect.crop_w = x2 - x1;
    rect.crop_h = y2 - y1;
    st = bmcv_image_draw_rectangle(h, p_->draw_img, 1, &rect, 3, 0, 255, 0);
    if (st != BM_SUCCESS) { err = "BMCV draw: draw_rectangle 失败"; return false; }
  }

  // 3) D2H: 写回 AVFrame NV12（in-place，编码器可见）
  void* dst_planes[2] = {frame->data[0], frame->data[1]};
  st = bm_image_copy_device_to_host(p_->draw_img, dst_planes);
  if (st != BM_SUCCESS) { err = "BMCV draw: D2H 失败"; return false; }
  return true;
}

}  // namespace hzw
