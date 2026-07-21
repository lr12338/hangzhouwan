// -*- coding: utf-8 -*-
// =============================================================================
// 极简 JPEG 读写与绘图工具（阶段3 单图 PoC 用）。
//
// 说明：
//   - 板端 sophon-opencv 仅为运行时库（无 C++ 头文件），系统 OpenCV 依赖过重，
//     因此本模块基于 libjpeg-turbo 实现 JPEG 解码/编码，并自实现 resize/绘框/绘字，
//     保持预处理语义与原 Python（cv2）一致：RGB / 直接 resize / 除以255 / NCHW。
//   - Image 始终为 RGB、8 位、行主序（第 0 行在顶部）。
// =============================================================================
#ifndef HZW_IMAGE_IO_JPEG_IO_H
#define HZW_IMAGE_IO_JPEG_IO_H

#include <cstdint>
#include <string>
#include <vector>

namespace hzw {

struct Color {
  uint8_t r = 0;
  uint8_t g = 255;
  uint8_t b = 0;
};

// RGB 8 位图像，行主序，channels 固定为 3。
struct Image {
  int width = 0;
  int height = 0;
  static constexpr int channels = 3;
  std::vector<uint8_t> data;  // size = width * height * 3

  bool valid() const {
    return width > 0 && height > 0 &&
           static_cast<int>(data.size()) == width * height * channels;
  }
  // 返回指向像素 (x,y) 的指针（3 字节 RGB）。越界返回 nullptr。
  uint8_t* pixel(int x, int y);
  const uint8_t* pixel(int x, int y) const;
};

// 解码 JPEG 文件为 RGB Image。失败返回 false。
bool decode_jpeg(const std::string& path, Image& out);

// 将 RGB Image 编码为 JPEG 文件。quality 1-100。失败返回 false。
bool encode_jpeg(const std::string& path, const Image& img, int quality = 92);

// 双线性缩放（半像素中心对齐，尽量贴近 cv2.resize INTER_LINEAR）。
// 输入/输出均为 RGB。失败（输入无效或目标尺寸非法）返回 false。
bool resize_bilinear(const Image& src, int dst_w, int dst_h, Image& dst);

// 绘制矩形边框（RGB）。
void draw_rectangle(Image& img, int x1, int y1, int x2, int y2,
                    Color color, int thickness = 2);

// 绘制文本（内置 5x7 位图字体，仅覆盖标签所需字符）。左上角对齐 (x, y)。
void draw_text(Image& img, int x, int y, const std::string& text,
               Color color, int scale = 2);

// 在 (x1,y1) 上方绘制填充背景的标签条并写文字，便于在原图上可读。
void draw_label(Image& img, int x1, int y1, const std::string& text,
                Color box_color, int scale = 2);

}  // namespace hzw

#endif  // HZW_IMAGE_IO_JPEG_IO_H
