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

  bm_image src_img{};     // NV12 (DDR1)，CSC/resize 输入
  bm_image csc_img{};     // RGB_PACKED 640x640 (DDR0)，CSC+resize 输出
  bm_image draw_img{};    // NV12 (DDR0)，绘制用
  bm_image crop_rgb{};    // RGB_PACKED (crop 输出，按需创建)
  bool crop_rgb_valid = false;
  int crop_w = 0;
  int crop_h = 0;
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
    if (p_->crop_rgb_valid) {
      bm_image_destroy(p_->crop_rgb);
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
  if (p_->crop_rgb_valid) {
    bm_image_destroy(p_->crop_rgb);
    p_->crop_rgb_valid = false;
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

bool BmcvProcessor::crop_to_rgb(AVFrame* frame, int x, int y, int crop_w, int crop_h,
                               Image& out, std::string& err, bool auto_upscale) {
  if (!p_ || !p_->ready) { err = "BMCV crop: 未初始化"; return false; }
  if (!frame) { err = "BMCV crop: frame 为空"; return false; }
  if (crop_w <= 0 || crop_h <= 0) { err = "BMCV crop: 尺寸非法"; return false; }
  // BMCV VPP 对极小输出尺寸不支持（实测 <32px 报 vpp dst width not match）。
  // 默认 32px；当 bbox 小于 min_out_dim 时按请求方向（auto_upscale）扩展源 crop，
  // 使输出 >= min_out_dim；否则返回失败由上层处理。
  int min_out_dim = 32;
  if (min_out_dim < 2) min_out_dim = 2;  // NV12 色度 2x 下采样要求偶数
  // 自动扩展到 min_out_dim：保持原 bbox 居中，向四周取上下文。
  if (auto_upscale && (crop_w < min_out_dim || crop_h < min_out_dim)) {
    int pad_w = (min_out_dim - crop_w) / 2;
    int pad_h = (min_out_dim - crop_h) / 2;
    int new_x = x - pad_w;
    int new_y = y - pad_h;
    int new_w = crop_w + 2 * pad_w;
    int new_h = crop_h + 2 * pad_h;
    // 优先让较长的一边先达到 min_out_dim；
    // 若源分辨率不足，按比例缩放以满足 min_out_dim。
    if (new_w < min_out_dim) {
      int extra = min_out_dim - new_w;
      new_w = min_out_dim;
      new_x -= extra / 2;
    }
    if (new_h < min_out_dim) {
      int extra = min_out_dim - new_h;
      new_h = min_out_dim;
      new_y -= extra / 2;
    }
    // clamp 到源边界
    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    if (new_x + new_w > p_->src_w) new_w = p_->src_w - new_x;
    if (new_y + new_h > p_->src_h) new_h = p_->src_h - new_y;
    if (new_w < min_out_dim) new_w = std::min(min_out_dim, p_->src_w - new_x);
    if (new_h < min_out_dim) new_h = std::min(min_out_dim, p_->src_h - new_y);
    if (new_w >= min_out_dim && new_h >= min_out_dim && new_x >= 0 && new_y >= 0) {
      x = new_x; y = new_y; crop_w = new_w; crop_h = new_h;
    }
  }
  // clamp 到源边界
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x + crop_w > p_->src_w) crop_w = p_->src_w - x;
  if (y + crop_h > p_->src_h) crop_h = p_->src_h - y;
  // NV12 色度 2x 下采样：crop 起点/尺寸必须偶数对齐，否则 VPP 报错。
  x = x & ~1;
  y = y & ~1;
  crop_w = crop_w & ~1;
  crop_h = crop_h & ~1;
  if (crop_w <= 0 || crop_h <= 0) { err = "BMCV crop: 裁剪区域越界"; return false; }
  if (crop_w < min_out_dim || crop_h < min_out_dim) {
    err = "BMCV crop: 裁剪区域过小（已尝试 auto_upscale）";
    return false;
  }

  bm_handle_t h = p_->handle;
  bm_status_t st;

  // 1) H2D: AVFrame NV12 -> 源 bm_image
  void* host_planes[2] = {frame->data[0], frame->data[1]};
  st = bm_image_copy_host_to_device(p_->src_img, host_planes);
  if (st != BM_SUCCESS) { err = "BMCV crop: H2D 失败"; return false; }

  // 2) 按需（重）创建 crop 输出 RGB_PACKED bm_image
  if (p_->crop_rgb_valid && (p_->crop_w != crop_w || p_->crop_h != crop_h)) {
    bm_image_destroy(p_->crop_rgb);
    p_->crop_rgb_valid = false;
  }
  if (!p_->crop_rgb_valid) {
    int stride[1] = {crop_w * 3};
    st = bm_image_create(h, crop_h, crop_w, FORMAT_RGB_PACKED, DATA_TYPE_EXT_1N_BYTE,
                         &p_->crop_rgb, stride);
    if (st != BM_SUCCESS) { err = "BMCV crop: 创建输出失败"; return false; }
    st = bm_image_alloc_dev_mem(p_->crop_rgb, BMCV_HEAP0_ID);
    if (st != BM_SUCCESS) { bm_image_destroy(p_->crop_rgb); err = "BMCV crop: 分配设备内存失败"; return false; }
    p_->crop_w = crop_w;
    p_->crop_h = crop_h;
    p_->crop_rgb_valid = true;
  }

  // 3) BMCV VPP: crop(src roi) + CSC NV12->RGB_PACKED 一步完成
  //    crop_rect 指定源裁剪区域；输出 bm_image 尺寸 = crop_w x crop_h（无缩放）。
  bmcv_rect_t crop_rect;
  crop_rect.start_x = x;
  crop_rect.start_y = y;
  crop_rect.crop_w = crop_w;
  crop_rect.crop_h = crop_h;
  st = bmcv_image_vpp_convert(h, 1, p_->src_img, &p_->crop_rgb, &crop_rect,
                              BMCV_INTER_LINEAR);
  if (st != BM_SUCCESS) { err = "BMCV crop: VPP convert 失败"; return false; }

  // 4) D2H: RGB_PACKED -> host Image
  out.width = crop_w;
  out.height = crop_h;
  out.data.assign(static_cast<size_t>(crop_w) * crop_h * 3, 0);
  void* crop_host[1] = {out.data.data()};
  st = bm_image_copy_device_to_host(p_->crop_rgb, crop_host);
  if (st != BM_SUCCESS) { err = "BMCV crop: D2H 失败"; return false; }
  return out.valid();
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

  // h264_bm 的 is_dma_buffer=1 只接受符合厂商契约的连续物理帧。应用自行
  // 创建的 bm_image 不能通过手工填充 data[4..6] 冒充厂商 AVFrame。
  // 先建立标准 host AVFrame，再让 BMCV 输出采用相同步长并显式回拷。
  AVFrame* out = av_frame_alloc();
  if (!out) {
    err = "BMCV resize: 分配 AVFrame 失败";
    return false;
  }
  out->format = AV_PIX_FMT_YUV420P;
  out->width = output_width;
  out->height = output_height;
  if (av_frame_get_buffer(out, 64) < 0 || av_frame_make_writable(out) < 0) {
    av_frame_free(&out);
    err = "BMCV resize: 分配主机帧缓冲失败";
    return false;
  }

  int stride[3] = {out->linesize[0], out->linesize[1], out->linesize[2]};
  bm_image output_image{};
  bm_status_t st = bm_image_create(
      p_->handle, output_height, output_width, FORMAT_YUV420P,
      DATA_TYPE_EXT_1N_BYTE, &output_image, stride);
  if (st != BM_SUCCESS) {
    av_frame_free(&out);
    err = "BMCV resize: 创建输出 YUV420P 失败";
    return false;
  }
  st = bm_image_alloc_dev_mem(output_image, BMCV_HEAP0_ID);
  if (st != BM_SUCCESS) {
    bm_image_destroy(output_image);
    av_frame_free(&out);
    err = "BMCV resize: 分配输出设备内存失败";
    return false;
  }

  // 每帧只做一次 H2D，先在设备上缩放。矩形稍后按比例映射到输出图像
  // 批量绘制，避免在 2560x1440 原图上逐框渲染。
  void* host_planes[2] = {frame->data[0], frame->data[1]};
  st = bm_image_copy_host_to_device(p_->src_img, host_planes);
  if (st != BM_SUCCESS) {
    bm_image_destroy(output_image);
    av_frame_free(&out);
    err = "BMCV resize: H2D 失败";
    return false;
  }

  st = bmcv_image_vpp_convert(p_->handle, 1, p_->src_img, &output_image,
                              nullptr, BMCV_INTER_LINEAR);
  if (st != BM_SUCCESS) {
    bm_image_destroy(output_image);
    av_frame_free(&out);
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
          p_->handle, output_image, static_cast<int>(batch.rects.size()),
          batch.rects.data(), 2, batch.r, batch.g, batch.b);
      if (st != BM_SUCCESS) {
        bm_image_destroy(output_image);
        av_frame_free(&out);
        err = "BMCV resize: 输出设备矩形绘制失败";
        return false;
      }
    }
  }

  void* output_planes[3] = {out->data[0], out->data[1], out->data[2]};
  st = bm_image_copy_device_to_host(output_image, output_planes);
  bm_image_destroy(output_image);
  if (st != BM_SUCCESS) {
    av_frame_free(&out);
    err = "BMCV resize: D2H 失败";
    return false;
  }
  *output = out;
  return true;
}

}  // namespace hzw
