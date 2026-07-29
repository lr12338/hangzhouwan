// -*- coding: utf-8 -*-
// 禁区过滤单元测试：中心点包含关系、矩形/多边形判断、分辨率换算。
// 语义与旧 Python method.py 一致。纯逻辑，不依赖硬件。
#include <iostream>
#include <vector>
#include "video/detection_region_filter.h"

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cerr << "失败 | " << #cond << " @行" << __LINE__ << "\n";    \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

int main() {
  using hzw::Detection;
  using hzw::DetectionRegionFilter;
  using hzw::Point;
  using hzw::Rect;

  // 1) 矩形禁区：中心点在禁区内则过滤
  {
    DetectionRegionFilter f;
    std::vector<Rect> rects = {{1480, 0, 2560, 630}};
    f.configure(rects, {}, 2560, 1440, 2560, 1440);
    CHECK(f.enabled());
    Detection d1{0, "ship", 0.9f, 1900, 200, 2100, 400};  // 中心(2000,300)在禁区内
    Detection d2{0, "ship", 0.9f, 400, 200, 600, 400};    // 中心(500,300)不在
    std::vector<Detection> dets = {d1, d2};
    f.filter(dets);
    CHECK(dets.size() == 1);
    CHECK(dets[0].x1 == 400);
  }

  // 2) 多边形禁区：射线法
  {
    DetectionRegionFilter f;
    std::vector<std::vector<Point>> polys = {{{0, 0}, {0, 640}, {640, 0}}};
    f.configure({}, polys, 640, 640, 640, 640);
    Detection d1{0, "ship", 0.9f, 50, 50, 150, 150};    // 中心(100,100)在三角形内
    Detection d2{0, "ship", 0.9f, 450, 450, 550, 550};  // 中心(500,500)在外
    std::vector<Detection> dets = {d1, d2};
    f.filter(dets);
    CHECK(dets.size() == 1);
    CHECK(dets[0].x1 == 450);
  }

  // 3) 分辨率换算：禁区坐标参考 2560x1440，实际 1280x720
  {
    DetectionRegionFilter f;
    std::vector<Rect> rects = {{1480, 0, 2560, 630}};
    f.configure(rects, {}, 2560, 1440, 1280, 720);
    // 换算后禁区 (740,0)-(1280,315)
    CHECK(f.is_in_forbidden(1000, 150));
    CHECK(!f.is_in_forbidden(500, 150));
  }

  // 4) 未配置不启用
  {
    DetectionRegionFilter f;
    CHECK(!f.enabled());
    std::vector<Detection> dets = {{0, "ship", 0.9f, 100, 100, 200, 200}};
    f.filter(dets);
    CHECK(dets.size() == 1);
  }

  // 5) 边界点：矩形含边界
  {
    DetectionRegionFilter f;
    std::vector<Rect> rects = {{100, 100, 200, 200}};
    f.configure(rects, {}, 200, 200, 200, 200);
    CHECK(f.is_in_forbidden(100, 100));
    CHECK(f.is_in_forbidden(200, 200));
    CHECK(!f.is_in_forbidden(201, 200));
  }

  // 6) 多个禁区：任一命中即过滤
  {
    DetectionRegionFilter f;
    std::vector<Rect> rects = {{0, 0, 100, 100}, {200, 200, 300, 300}};
    f.configure(rects, {}, 300, 300, 300, 300);
    CHECK(f.is_in_forbidden(50, 50));
    CHECK(f.is_in_forbidden(250, 250));
    CHECK(!f.is_in_forbidden(150, 150));
  }

  if (g_failures == 0) std::cout << "通过 | 禁区过滤单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
