// -*- coding: utf-8 -*-
// =============================================================================
// StreamProfile：单路业务配置（阶段4.3 原业务配置迁移）。
//
// 字段：stream_id, input_url, output_url, coordinate_model_path, camera_param,
//       forbidden_rectangles, forbidden_polygons。
//
// 当前阶段：加载并保存 A/B 配置，输出启动时 stream_id，为后续禁区过滤和坐标预测准备接口。
// 暂不执行坐标预测、AIS 关联、船名查询、MQTT。
//
// 解析器为轻量行扫描（不依赖外部 YAML 库），仅处理 internal-development.yaml 所需字段。
// 纯逻辑，可独立单元测试。
// =============================================================================
#ifndef HZW_CONFIG_STREAM_PROFILE_H
#define HZW_CONFIG_STREAM_PROFILE_H

#include <cstdint>
#include <string>
#include <vector>
#include "video/detection_region_filter.h"  // Rect, Point

namespace hzw {

struct StreamProfile {
  std::string stream_id;
  std::string input_url;
  std::string output_url;
  std::string coordinate_model_path;
  double camera_param = 0.0;
  std::vector<Rect> forbidden_rectangles;
  std::vector<std::vector<Point>> forbidden_polygons;
  bool enabled = false;
};

// 从 internal-development.yaml 内容解析指定 stream_id 的 StreamProfile。
// 返回 false 表示未找到该 stream_id 或解析失败（err 给出原因）。
// 内部开发阶段：允许 URL 等真实值出现在配置文件中，但不写入 Git。
bool parse_stream_profile(const std::string& yaml_content,
                          const std::string& stream_id,
                          StreamProfile& out, std::string& err);

// 校验 StreamProfile 必填字段（stream_id, input_url, output_url）。
bool validate_stream_profile(const StreamProfile& p, std::string& err);

}  // namespace hzw

#endif  // HZW_CONFIG_STREAM_PROFILE_H
