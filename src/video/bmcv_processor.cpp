// -*- coding: utf-8 -*-
#include "video/bmcv_processor.h"

#include <cstdio>
#include <cstring>
#include <new>

extern "C" {
#include "bmlib_runtime.h"
#include "bmcv_api_ext.h"
}

namespace hzw {

namespace {

// h264_bm 的 DMA 输入契约使用 data[4..6] 承载设备物理地址，同时仍要求
// AVFrame 带有可引用的 buf[]。Owner 由 buf[0] 的释放回调持有，确保编码器
// 延迟引用帧期间 bm_image 不会被下一帧复用或提前释放。
struct DmaFrameOwner {
  bm_image image{};
  bool image_valid = false;
  uint8_t* host_placeholder = nullptr;
};

void free_dma_frame(void* opaque, uint8_t*) {
  auto* owner = static_cast<DmaFrameOwner*>(opaque);
  if (!owner) return;
  if (owner->image_valid) bm_image_destroy(owner->image);
  av_free(owner->host_placeholder);
  delete owner;
}

void free_dma_plane_ref(void*, uint8_t*) {}

}  // namespace

struct BmcvProcessor::Impl {
  bm_handle_t handle = nullptr;
  int src_w = 0;
  int src_h = 0;

  bm_image src_img{};     // NV12 (DDR1)，CSC/resize 输入
  bm_image csc_img{};     // RGB_PACKED 640x640 (DDR0)，CSC+resize 输出
  bm_image draw_img{};    // NV12 (DDR0)，绘制用
  bool images_valid = false;
  bool use_vpp_resize = false;  // 是否使用 VPP resize（640x640 输出）

  std::vector<uint8_t> rgb_640;  // RGB_PACKED 640x640 (host)

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
  p_->handle = static_cast<bm_handle_t>(handle_void);
  p_->src_w = src_w;
  p_->src_h = src_h;
  if (!p_->handle) { err = "BMCV: 设备句柄为空"; return false; }
  if (src_w <= 0 || src_h <= 0) { err = "BMCV: 源尺寸非法"; return false; }

  bm_handle_t h = p_->handle;
  bm_status_t st;

  // 源 NV12 bm_image（DDR1，CSC/VPP 输入要求 DDR1）。
  int nv12_stride = src_w;
  int src_stride[2] = {nv12_stride, nv12_stride};
  st = bm_image_create(h, src_h, src_w, FORMAT_NV12, DATA_TYPE_EXT_1N_BYTE,
                       &p_->src_img, src_stride);
  if (st != BM_SUCCESS) { err = "BMCV: 创建源 NV12 失败"; return false; }
  st = bm_image_alloc_dev_mem(p_->src_img, BMCV_HEAP1_ID);
  if (st != BM_SUCCESS) { bm_image_destroy(p_->src_img); err = "BMCV: 分配源设备内存失败"; return false; }

  // CSC+resize 输出：RGB_PACKED 640x640（DDR0）。
  // 使用 bmcv_image_vpp_convert 在设备上同时完成 CSC 和 resize，
  // 避免 D2H 全分辨率 RGB（2560x1440 -> 11MB）和 CPU sws_scale。
  int csc_stride[1] = {640 * 3};
  st = bm_image_create(h, 640, 640, FORMAT_RGB_PACKED, DATA_TYPE_EXT_1N_BYTE,
                       &p_->csc_img, csc_stride);
  if (st != BM_SUCCESS) { bm_image_destroy(p_->src_img); err = "BMCV: 创建 CSC 输出失败"; return false; }
  st = bm_image_alloc_dev_mem(p_->csc_img, BMCV_HEAP0_ID);
  if (st != BM_SUCCESS) { bm_image_destroy(p_->src_img); bm_image_destroy(p_->csc_img); err = "BMCV: 分配 CSC 设备内存失败"; return false; }
  p_->use_vpp_resize = true;

  p_->rgb_640.assign(static_cast<size_t>(640) * 640 * 3, 0);

  // 绘制：NV12 bm_image（DDR0）
  int draw_stride[2] = {nv12_stride, nv12_stride};
  st = bm_image_create(h, src_h, src_w, FORMAT_NV12, DATA_TYPE_EXT_1N_BYTE,
                       &p_->draw_img, draw_stride);
  if (st != BM_SUCCESS) {
    bm_image_destroy(p_->src_img); bm_image_destroy(p_->csc_img);
    err = "BMCV: 创建绘制 NV12 失败"; return false;
  }
  st = bm_image_alloc_dev_mem(p_->draw_img, BMCV_HEAP0_ID);
  if (st != BM_SUCCESS) {
    bm_image_destroy(p_->src_img); bm_image_destroy(p_->csc_img);
    bm_image_destroy(p_->draw_img);
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

  // 2) BMCV VPP: NV12 src_w x src_h -> RGB_PACKED 640x640（CSC + resize 一步完成）
  //    D2H 仅需 1.2MB（640x640x3），远小于全分辨率 11MB。
  st = bmcv_image_vpp_convert(h, 1, p_->src_img, &p_->csc_img, nullptr, BMCV_INTER_LINEAR);
  if (st != BM_SUCCESS) { err = "BMCV preprocess: VPP convert 失败"; return false; }

  // 3) D2H: RGB_PACKED 640x640 -> host
  void* csc_host[1] = {p_->rgb_640.data()};
  st = bm_image_copy_device_to_host(p_->csc_img, csc_host);
  if (st != BM_SUCCESS) { err = "BMCV preprocess: D2H 失败"; return false; }

  // 4) CPU 归一化：RGB_PACKED 640x640 -> FLOAT32 NCHW [1,3,640,640]，除以 255
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
  if (dets.empty()) return true;

  std::vector<ColoredRect> rects;
  rects.reserve(dets.size());
  for (const auto& d : dets) {
    ColoredRect r;
    r.x1 = d.x1; r.y1 = d.y1; r.x2 = d.x2; r.y2 = d.y2;
    r.r = 0; r.g = 255; r.b = 0;  // 绿色
    rects.push_back(r);
  }
  return draw_colored_rectangles(frame, rects, err);
}

bool BmcvProcessor::draw_colored_rectangles(AVFrame* frame,
                                            const std::vector<ColoredRect>& rects,
                                            std::string& err) {
  if (!p_ || !p_->ready) { err = "BMCV: 未初始化"; return false; }
  if (rects.empty()) return true;

  bm_handle_t h = p_->handle;
  bm_status_t st;

  // 1) H2D: AVFrame NV12 -> 绘制 bm_image
  void* host_planes[2] = {frame->data[0], frame->data[1]};
  st = bm_image_copy_host_to_device(p_->draw_img, host_planes);
  if (st != BM_SUCCESS) { err = "BMCV draw: H2D 失败"; return false; }

  // 2) BMCV draw_rectangle（线宽 3，按状态着色）
  for (const auto& r : rects) {
    int x1 = static_cast<int>(r.x1);
    int y1 = static_cast<int>(r.y1);
    int x2 = static_cast<int>(r.x2);
    int y2 = static_cast<int>(r.y2);
    if (x2 <= x1 || y2 <= y1) continue;
    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 > p_->src_w) x2 = p_->src_w; if (y2 > p_->src_h) y2 = p_->src_h;
    bmcv_rect_t rect;
    rect.start_x = x1;
    rect.start_y = y1;
    rect.crop_w = x2 - x1;
    rect.crop_h = y2 - y1;
    st = bmcv_image_draw_rectangle(h, p_->draw_img, 1, &rect, 3, r.r, r.g, r.b);
    if (st != BM_SUCCESS) { err = "BMCV draw: draw_rectangle 失败"; return false; }
  }

  // 3) D2H: 写回 AVFrame NV12（in-place，编码器可见）
  void* dst_planes[2] = {frame->data[0], frame->data[1]};
  st = bm_image_copy_device_to_host(p_->draw_img, dst_planes);
  if (st != BM_SUCCESS) { err = "BMCV draw: D2H 失败"; return false; }
  return true;
}

bool BmcvProcessor::resize_yuv420p(AVFrame* frame, int output_width,
                                   int output_height, AVFrame** output,
                                   std::string& err,
                                   const std::vector<ColoredRect>* overlays) {
  if (!p_ || !p_->ready) { err = "BMCV: 未初始化"; return false; }
  if (!frame || !output || output_width <= 0 || output_height <= 0 ||
      (output_width % 2) != 0 || (output_height % 2) != 0) {
    err = "BMCV resize: 参数非法";
    return false;
  }
  *output = nullptr;
  auto* owner = new (std::nothrow) DmaFrameOwner();
  if (!owner) {
    err = "BMCV resize: 分配 DMA 帧所有者失败";
    return false;
  }

  int stride[3] = {output_width, output_width / 2, output_width / 2};
  bm_status_t st = bm_image_create(
      p_->handle, output_height, output_width, FORMAT_YUV420P,
      DATA_TYPE_EXT_1N_BYTE, &owner->image, stride);
  if (st != BM_SUCCESS) {
    delete owner;
    err = "BMCV resize: 创建输出 YUV420P 失败";
    return false;
  }
  owner->image_valid = true;
  st = bm_image_alloc_dev_mem(owner->image, BMCV_HEAP0_ID);
  if (st != BM_SUCCESS) {
    bm_image_destroy(owner->image);
    delete owner;
    err = "BMCV resize: 分配输出设备内存失败";
    return false;
  }

  // 每帧只做一次 H2D，先在设备上缩放。矩形稍后按比例映射到输出图像
  // 批量绘制，避免在 2560x1440 原图上逐框渲染。
  void* host_planes[2] = {frame->data[0], frame->data[1]};
  st = bm_image_copy_host_to_device(p_->src_img, host_planes);
  if (st != BM_SUCCESS) {
    bm_image_destroy(owner->image);
    delete owner;
    err = "BMCV resize: H2D 失败";
    return false;
  }

  st = bmcv_image_vpp_convert(p_->handle, 1, p_->src_img, &owner->image,
                              nullptr, BMCV_INTER_LINEAR);
  if (st != BM_SUCCESS) {
    bm_image_destroy(owner->image);
    delete owner;
    err = "BMCV resize: VPP 缩放失败";
    return false;
  }

  if (overlays && !overlays->empty()) {
    struct RectBatch {
      unsigned char r;
      unsigned char g;
      unsigned char b;
      std::vector<bmcv_rect_t> rects;
    };
    RectBatch batches[3] = {
        {0, 255, 0, {}}, {255, 255, 0, {}}, {255, 0, 0, {}}};
    const float scale_x =
        static_cast<float>(output_width) / static_cast<float>(p_->src_w);
    const float scale_y =
        static_cast<float>(output_height) / static_cast<float>(p_->src_h);
    for (const auto& r : *overlays) {
      int x1 = static_cast<int>(r.x1 * scale_x);
      int y1 = static_cast<int>(r.y1 * scale_y);
      int x2 = static_cast<int>(r.x2 * scale_x);
      int y2 = static_cast<int>(r.y2 * scale_y);
      if (x1 < 0) x1 = 0;
      if (y1 < 0) y1 = 0;
      if (x2 > output_width) x2 = output_width;
      if (y2 > output_height) y2 = output_height;
      if (x2 <= x1 || y2 <= y1) continue;
      bmcv_rect_t rect{x1, y1, x2 - x1, y2 - y1};
      RectBatch* batch = &batches[2];
      if (r.r == 0 && r.g == 255 && r.b == 0) batch = &batches[0];
      else if (r.r == 255 && r.g == 255 && r.b == 0) batch = &batches[1];
      batch->rects.push_back(rect);
    }
    for (auto& batch : batches) {
      if (batch.rects.empty()) continue;
      st = bmcv_image_draw_rectangle(
          p_->handle, owner->image, static_cast<int>(batch.rects.size()),
          batch.rects.data(), 2, batch.r, batch.g, batch.b);
      if (st != BM_SUCCESS) {
        bm_image_destroy(owner->image);
        delete owner;
        err = "BMCV resize: 输出设备矩形绘制失败";
        return false;
      }
    }
  }

  AVFrame* out = av_frame_alloc();
  if (!out) {
    bm_image_destroy(owner->image);
    delete owner;
    err = "BMCV resize: 分配 AVFrame 失败";
    return false;
  }
  out->format = AV_PIX_FMT_YUV420P;
  out->width = output_width;
  out->height = output_height;

  bm_device_mem_t device_mem[3]{};
  bm_image_format_info_t image_info{};
  if (bm_image_get_device_mem(owner->image, device_mem) != BM_SUCCESS ||
      bm_image_get_format_info(&owner->image, &image_info) != BM_SUCCESS) {
    av_frame_free(&out);
    bm_image_destroy(owner->image);
    delete owner;
    err = "BMCV resize: 读取设备平面信息失败";
    return false;
  }

  // 与 Sophon 官方 bm_image_to_avframe 契约一致：buf[] 只负责引用生命周期，
  // 编码器在 is_dma_buffer=1 时从 data[4..6] 读取设备地址。
  const int y_bytes = output_width * output_height;
  const int chroma_bytes = y_bytes / 4;
  owner->host_placeholder =
      static_cast<uint8_t*>(av_malloc(static_cast<size_t>(y_bytes) * 3 / 2));
  if (!owner->host_placeholder) {
    av_frame_free(&out);
    bm_image_destroy(owner->image);
    delete owner;
    err = "BMCV resize: 分配 AVFrame 引用占位缓冲失败";
    return false;
  }

  uint8_t* y = owner->host_placeholder;
  uint8_t* u = y + y_bytes;
  uint8_t* v = u + chroma_bytes;
  out->buf[0] = av_buffer_create(
      y, y_bytes, free_dma_frame, owner, AV_BUFFER_FLAG_READONLY);
  if (!out->buf[0]) {
    bm_image_destroy(owner->image);
    av_free(owner->host_placeholder);
    delete owner;
    av_frame_free(&out);
    err = "BMCV resize: 创建 DMA 帧主引用失败";
    return false;
  }
  // 从此处起 owner 所有权已转交给 out->buf[0]。
  out->buf[1] = av_buffer_create(
      u, chroma_bytes, free_dma_plane_ref, nullptr, AV_BUFFER_FLAG_READONLY);
  out->buf[2] = av_buffer_create(
      v, chroma_bytes, free_dma_plane_ref, nullptr, AV_BUFFER_FLAG_READONLY);
  if (!out->buf[1] || !out->buf[2]) {
    av_frame_free(&out);
    err = "BMCV resize: 创建 DMA 帧平面引用失败";
    return false;
  }
  out->data[0] = y;
  out->data[1] = u;
  out->data[2] = v;
  for (int i = 0; i < 3; ++i) {
    out->data[4 + i] = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(bm_mem_get_device_addr(device_mem[i])));
    out->linesize[i] = image_info.stride[i];
    out->linesize[4 + i] = image_info.stride[i];
  }
  *output = out;
  return true;
}

}  // namespace hzw
