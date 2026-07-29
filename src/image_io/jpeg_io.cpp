// -*- coding: utf-8 -*-
#include "image_io/jpeg_io.h"

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>

extern "C" {
#include <jpeglib.h>
}

namespace hzw {

uint8_t* Image::pixel(int x, int y) {
  if (x < 0 || y < 0 || x >= width || y >= height) return nullptr;
  return data.data() + (static_cast<size_t>(y) * width + x) * channels;
}
const uint8_t* Image::pixel(int x, int y) const {
  if (x < 0 || y < 0 || x >= width || y >= height) return nullptr;
  return data.data() + (static_cast<size_t>(y) * width + x) * channels;
}

// ---- libjpeg 错误处理 ----
namespace {
struct JpegErrorMgr {
  jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
  std::string message;
};
void jpeg_error_exit(j_common_ptr cinfo) {
  JpegErrorMgr* myerr = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
  char buf[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message)(cinfo, buf);
  myerr->message = buf;
  std::longjmp(myerr->setjmp_buffer, 1);
}
}  // namespace

bool decode_jpeg(const std::string& path, Image& out) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;

  jpeg_decompress_struct cinfo;
  JpegErrorMgr jerr;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_error_exit;
  if (setjmp(jerr.setjmp_buffer)) {
    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);
    return false;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, fp);
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);
    return false;
  }
  cinfo.out_color_space = JCS_RGB;  // 直接输出 RGB
  if (!jpeg_start_decompress(&cinfo)) {
    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);
    return false;
  }
  out.width = static_cast<int>(cinfo.output_width);
  out.height = static_cast<int>(cinfo.output_height);
  out.data.resize(static_cast<size_t>(out.width) * out.height * Image::channels);
  const int row_stride = out.width * Image::channels;
  while (cinfo.output_scanline < static_cast<JDIMENSION>(out.height)) {
    uint8_t* row = out.data.data() +
                   static_cast<size_t>(cinfo.output_scanline) * row_stride;
    jpeg_read_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  std::fclose(fp);
  return out.valid();
}

bool encode_jpeg(const std::string& path, const Image& img, int quality) {
  if (!img.valid()) return false;
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;

  jpeg_compress_struct cinfo;
  JpegErrorMgr jerr;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_error_exit;
  if (setjmp(jerr.setjmp_buffer)) {
    jpeg_destroy_compress(&cinfo);
    std::fclose(fp);
    return false;
  }
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, fp);
  cinfo.image_width = static_cast<JDIMENSION>(img.width);
  cinfo.image_height = static_cast<JDIMENSION>(img.height);
  cinfo.input_components = Image::channels;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, std::max(1, std::min(100, quality)), TRUE);
  jpeg_start_compress(&cinfo, TRUE);
  const int row_stride = img.width * Image::channels;
  while (cinfo.next_scanline < static_cast<JDIMENSION>(img.height)) {
    const uint8_t* row =
        img.data.data() + static_cast<size_t>(cinfo.next_scanline) * row_stride;
    jpeg_write_scanlines(&cinfo, const_cast<JSAMPROW*>(&row), 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  std::fclose(fp);
  return true;
}

// ---- 双线性缩放（半像素中心对齐）----
bool resize_bilinear(const Image& src, int dst_w, int dst_h, Image& dst) {
  if (!src.valid() || dst_w <= 0 || dst_h <= 0) return false;
  dst.width = dst_w;
  dst.height = dst_h;
  dst.data.resize(static_cast<size_t>(dst_w) * dst_h * Image::channels);

  const double scale_x = static_cast<double>(src.width) / dst_w;
  const double scale_y = static_cast<double>(src.height) / dst_h;
  const int C = Image::channels;
  for (int dy = 0; dy < dst_h; ++dy) {
    double src_y = (dy + 0.5) * scale_y - 0.5;
    int y0 = static_cast<int>(std::floor(src_y));
    int y1 = y0 + 1;
    double fy = src_y - y0;
    if (y0 < 0) { y0 = 0; fy = 0.0; }
    if (y1 >= src.height) { y1 = src.height - 1; }
    if (y0 > src.height - 1) { y0 = src.height - 1; }
    for (int dx = 0; dx < dst_w; ++dx) {
      double src_x = (dx + 0.5) * scale_x - 0.5;
      int x0 = static_cast<int>(std::floor(src_x));
      int x1 = x0 + 1;
      double fx = src_x - x0;
      if (x0 < 0) { x0 = 0; fx = 0.0; }
      if (x1 >= src.width) { x1 = src.width - 1; }
      if (x0 > src.width - 1) { x0 = src.width - 1; }
      const uint8_t* p00 = src.pixel(x0, y0);
      const uint8_t* p10 = src.pixel(x1, y0);
      const uint8_t* p01 = src.pixel(x0, y1);
      const uint8_t* p11 = src.pixel(x1, y1);
      uint8_t* d = dst.pixel(dx, dy);
      for (int c = 0; c < C; ++c) {
        double v = (1.0 - fx) * (1.0 - fy) * p00[c] +
                   fx * (1.0 - fy) * p10[c] +
                   (1.0 - fx) * fy * p01[c] +
                   fx * fy * p11[c];
        int iv = static_cast<int>(std::lround(v));
        d[c] = static_cast<uint8_t>(std::max(0, std::min(255, iv)));
      }
    }
  }
  return true;
}

void draw_rectangle(Image& img, int x1, int y1, int x2, int y2,
                    Color color, int thickness) {
  if (!img.valid()) return;
  int w = img.width, h = img.height;
  if (x2 < x1) std::swap(x1, x2);
  if (y2 < y1) std::swap(y1, y2);
  x1 = std::max(0, x1); y1 = std::max(0, y1);
  x2 = std::min(w - 1, x2); y2 = std::min(h - 1, y2);
  int t = std::max(1, thickness);
  for (int i = 0; i < t; ++i) {
    int yy1 = std::min(h - 1, y1 + i);
    int yy2 = std::min(h - 1, y2 - i);
    int xx1 = std::min(w - 1, x1 + i);
    int xx2 = std::min(w - 1, x2 - i);
    for (int x = xx1; x <= xx2; ++x) {
      uint8_t* p;
      if ((p = img.pixel(x, yy1))) { p[0] = color.r; p[1] = color.g; p[2] = color.b; }
      if ((p = img.pixel(x, yy2))) { p[0] = color.r; p[1] = color.g; p[2] = color.b; }
    }
    for (int y = yy1; y <= yy2; ++y) {
      uint8_t* p;
      if ((p = img.pixel(xx1, y))) { p[0] = color.r; p[1] = color.g; p[2] = color.b; }
      if ((p = img.pixel(xx2, y))) { p[0] = color.r; p[1] = color.g; p[2] = color.b; }
    }
  }
}

// ---- 内置 5x7 位图字体 ----
// 仅覆盖标签所需字符：空格 '0'-'9' '.' 's' 'h' 'i' 'p'。
// 每个字形 7 行，每行 5 位（bit4..bit0 对应左..右像素）。
namespace {
struct Glyph {
  char ch;
  uint8_t rows[7];
};

// 辅助：5 位宽字符串 -> 行字节
static uint8_t r(const char* s) {
  uint8_t v = 0;
  for (int i = 0; i < 5; ++i) v = (v << 1) | (s[i] == '#' ? 1 : 0);
  return v;
}

static const Glyph kFont[] = {
  {' ', {r("....."), r("....."), r("....."), r("....."), r("....."), r("....."), r(".....")}},
  {'.', {r("....."), r("....."), r("....."), r("....."), r("....."), r("..#.."), r("..#..")}},
  {'0', {r(".###."), r("#...#"), r("#..##"), r("#.#.#"), r("##..#"), r("#...#"), r(".###.")}},
  {'1', {r("..#.."), r(".##.."), r("..#.."), r("..#.."), r("..#.."), r("..#.."), r(".###.")}},
  {'2', {r(".###."), r("#...#"), r("....#"), r("...#."), r("..#.."), r(".#..."), r("#####")}},
  {'3', {r(".###."), r("#...#"), r("....#"), r("..##."), r("....#"), r("#...#"), r(".###.")}},
  {'4', {r("...#."), r("..##."), r(".#.#."), r("#..#."), r("#####"), r("...#."), r("...#.")}},
  {'5', {r("#####"), r("#...."), r("####."), r("....#"), r("....#"), r("#...#"), r(".###.")}},
  {'6', {r("..##."), r(".#..."), r("#...."), r("####."), r("#...#"), r("#...#"), r(".###.")}},
  {'7', {r("#####"), r("....#"), r("...#."), r("..#.."), r(".#..."), r(".#..."), r(".#...")}},
  {'8', {r(".###."), r("#...#"), r("#...#"), r(".###."), r("#...#"), r("#...#"), r(".###.")}},
  {'9', {r(".###."), r("#...#"), r("#...#"), r(".####"), r("....#"), r("...#."), r(".##..")}},
  {'s', {r(".####"), r("#...."), r("#...."), r(".###."), r("....#"), r("....#"), r("####.")}},
  {'h', {r("#...."), r("#...."), r("#...."), r("####."), r("#...#"), r("#...#"), r("#...#")}},
  {'i', {r("..#.."), r("....."), r(".##.."), r("..#.."), r("..#.."), r("..#.."), r(".###.")}},
  {'p', {r("####."), r("#...#"), r("#...#"), r("####."), r("#...."), r("#...."), r("#....")}},
  // 大写字母 + 标点（融合信息标签用）
  {':', {r("....."), r("..#.."), r("..#.."), r("....."), r("..#.."), r("..#.."), r(".....")}},
  {'-', {r("....."), r("....."), r("....."), r("#####"), r("....."), r("....."), r(".....")}},
  {'A', {r(".###."), r("#...#"), r("#...#"), r("#####"), r("#...#"), r("#...#"), r("#...#")}},
  {'B', {r("####."), r("#...#"), r("#...#"), r("####."), r("#...#"), r("#...#"), r("####.")}},
  {'C', {r(".###."), r("#...#"), r("#...."), r("#...."), r("#...."), r("#...#"), r(".###.")}},
  {'D', {r("###.."), r("#..#."), r("#...#"), r("#...#"), r("#...#"), r("#..#."), r("###..")}},
  {'E', {r("#####"), r("#...."), r("#...."), r("####."), r("#...."), r("#...."), r("#####")}},
  {'F', {r("#####"), r("#...."), r("#...."), r("####."), r("#...."), r("#...."), r("#....")}},
  {'G', {r(".###."), r("#...#"), r("#...."), r("#.###"), r("#...#"), r("#...#"), r(".###.")}},
  {'H', {r("#...#"), r("#...#"), r("#...#"), r("#####"), r("#...#"), r("#...#"), r("#...#")}},
  {'I', {r(".###."), r("..#.."), r("..#.."), r("..#.."), r("..#.."), r("..#.."), r(".###.")}},
  {'J', {r("..###"), r("...#."), r("...#."), r("...#."), r("#..#."), r("#..#."), r(".###.")}},
  {'K', {r("#...#"), r("#..#."), r("#.#.."), r("##..."), r("#.#.."), r("#..#."), r("#...#")}},
  {'L', {r("#...."), r("#...."), r("#...."), r("#...."), r("#...."), r("#...."), r("#####")}},
  {'M', {r("#...#"), r("##.##"), r("#.#.#"), r("#.#.#"), r("#...#"), r("#...#"), r("#...#")}},
  {'N', {r("#...#"), r("##..#"), r("#.#.#"), r("#.#.#"), r("#.#.#"), r("#..##"), r("#...#")}},
  {'O', {r(".###."), r("#...#"), r("#...#"), r("#...#"), r("#...#"), r("#...#"), r(".###.")}},
  {'P', {r("####."), r("#...#"), r("#...#"), r("####."), r("#...."), r("#...."), r("#....")}},
  {'Q', {r(".###."), r("#...#"), r("#...#"), r("#...#"), r("#.#.#"), r("#..#."), r(".##.#")}},
  {'R', {r("####."), r("#...#"), r("#...#"), r("####."), r("#.#.."), r("#..#."), r("#...#")}},
  {'S', {r(".####"), r("#...."), r("#...."), r(".###."), r("....#"), r("....#"), r("####.")}},
  {'T', {r("#####"), r("..#.."), r("..#.."), r("..#.."), r("..#.."), r("..#.."), r("..#..")}},
  {'U', {r("#...#"), r("#...#"), r("#...#"), r("#...#"), r("#...#"), r("#...#"), r(".###.")}},
  {'V', {r("#...#"), r("#...#"), r("#...#"), r("#...#"), r("#...#"), r(".#.#."), r("..#..")}},
  {'W', {r("#...#"), r("#...#"), r("#...#"), r("#.#.#"), r("#.#.#"), r("##.##"), r("#...#")}},
  {'X', {r("#...#"), r("#...#"), r(".#.#."), r("..#.."), r(".#.#."), r("#...#"), r("#...#")}},
  {'Y', {r("#...#"), r("#...#"), r(".#.#."), r("..#.."), r("..#.."), r("..#.."), r("..#..")}},
  {'Z', {r("#####"), r("....#"), r("...#."), r("..#.."), r(".#..."), r("#...."), r("#####")}},
};
static const int kFontCount = sizeof(kFont) / sizeof(kFont[0]);

const Glyph* find_glyph(char c) {
  // 大小写不敏感：小写字母映射为大写（字体仅含大写形）
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  for (int i = 0; i < kFontCount; ++i)
    if (kFont[i].ch == c) return &kFont[i];
  // 未知字符用 '.' 占位
  for (int i = 0; i < kFontCount; ++i)
    if (kFont[i].ch == '.') return &kFont[i];
  return nullptr;
}
}  // namespace

void draw_text(Image& img, int x, int y, const std::string& text,
               Color color, int scale) {
  if (!img.valid() || scale < 1) return;
  int pen_x = x;
  for (char c : text) {
    const Glyph* g = find_glyph(c);
    if (!g) { pen_x += 6 * scale; continue; }
    for (int row = 0; row < 7; ++row) {
      uint8_t bits = g->rows[row];
      for (int col = 0; col < 5; ++col) {
        bool on = (bits >> (4 - col)) & 1;
        if (!on) continue;
        for (int sy = 0; sy < scale; ++sy) {
          for (int sx = 0; sx < scale; ++sx) {
            int px = pen_x + col * scale + sx;
            int py = y + row * scale + sy;
            if (uint8_t* p = img.pixel(px, py)) {
              p[0] = color.r; p[1] = color.g; p[2] = color.b;
            }
          }
        }
      }
    }
    pen_x += 6 * scale;  // 5 像素宽 + 1 像素间距
  }
}

static int text_width(const std::string& text, int scale) {
  return static_cast<int>(text.size()) * 6 * scale;
}
static int text_height(int scale) { return 7 * scale; }

void draw_label(Image& img, int x1, int y1, const std::string& text,
                Color box_color, int scale) {
  if (!img.valid()) return;
  int tw = text_width(text, scale);
  int th = text_height(scale);
  int lx = x1;
  int ly = y1 - th - 2;
  if (ly < 0) ly = y1 + 2;  // 顶部放不下则放到框内顶部
  // 背景条
  for (int y = ly; y < ly + th + 2 && y < img.height; ++y) {
    for (int x = lx; x < lx + tw + 2 && x < img.width; ++x) {
      if (uint8_t* p = img.pixel(x, y)) {
        p[0] = box_color.r; p[1] = box_color.g; p[2] = box_color.b;
      }
    }
  }
  // 文字（用对比色：框色为亮色时用黑字，否则白字）
  Color fg{0, 0, 0};
  int lum = (box_color.r * 299 + box_color.g * 587 + box_color.b * 114) / 1000;
  if (lum < 128) fg = Color{255, 255, 255};
  draw_text(img, lx + 1, ly + 1, text, fg, scale);
}

// 在 NV12 帧上直接绘制标签条（黑底白字，UV 置中性 128 为灰度）。
// 用于 BMCV 绘制路径：BMCV 仅能画矩形，文字需在 host NV12 上补绘。
void draw_label_nv12(uint8_t* y_plane, int y_stride,
                     uint8_t* uv_plane, int uv_stride,
                     int width, int height,
                     int x1, int y1, const std::string& text, int scale) {
  if (!y_plane || !uv_plane || scale < 1 || text.empty()) return;
  int tw = static_cast<int>(text.size()) * 6 * scale;
  int th = 7 * scale;
  int lx = x1;
  int ly = y1 - th - 2;
  if (ly < 0) ly = y1 + 2;  // 顶部放不下则放到框内顶部
  // 设置 Y 像素并将对应 UV 置为中性 128（灰度，避免偏色）
  auto set_y = [&](int x, int y, uint8_t val) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    y_plane[static_cast<size_t>(y) * y_stride + x] = val;
    int ux = x / 2, uy = y / 2;
    size_t uvi = static_cast<size_t>(uy) * uv_stride + ux * 2;
    uv_plane[uvi] = 128;
    uv_plane[uvi + 1] = 128;
  };
  // 背景条（黑）
  for (int y = ly; y < ly + th + 2 && y < height; ++y)
    for (int x = lx; x < lx + tw + 2 && x < width; ++x)
      set_y(x, y, 16);
  // 文字（白）
  int pen_x = lx + 1;
  for (char c : text) {
    const Glyph* g = find_glyph(c);
    if (!g) { pen_x += 6 * scale; continue; }
    for (int row = 0; row < 7; ++row) {
      uint8_t bits = g->rows[row];
      for (int col = 0; col < 5; ++col) {
        bool on = (bits >> (4 - col)) & 1;
        if (!on) continue;
        for (int sy = 0; sy < scale; ++sy)
          for (int sx = 0; sx < scale; ++sx)
            set_y(pen_x + col * scale + sx, ly + 1 + row * scale + sy, 235);
      }
    }
    pen_x += 6 * scale;
  }
}

}  // namespace hzw
