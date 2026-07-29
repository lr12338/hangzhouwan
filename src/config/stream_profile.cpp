// -*- coding: utf-8 -*-
#include "config/stream_profile.h"

#include <cctype>
#include <sstream>

namespace hzw {

namespace {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// 去除引号
std::string unquote(const std::string& s) {
  std::string t = trim(s);
  if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') ||
                         (t.front() == '\'' && t.back() == '\''))) {
    return t.substr(1, t.size() - 2);
  }
  return t;
}

// 解析 [x1, y1, x2, y2] 为 Rect
bool parse_rect(const std::string& s, Rect& out) {
  std::string t = s;
  // 去除方括号
  size_t lb = t.find('[');
  size_t rb = t.rfind(']');
  if (lb == std::string::npos || rb == std::string::npos || rb <= lb) return false;
  t = t.substr(lb + 1, rb - lb - 1);
  std::stringstream ss(t);
  std::string item;
  std::vector<double> vals;
  while (std::getline(ss, item, ',')) {
    try {
      vals.push_back(std::stod(trim(item)));
    } catch (...) {
      return false;
    }
  }
  if (vals.size() != 4) return false;
  out.x1 = vals[0]; out.y1 = vals[1]; out.x2 = vals[2]; out.y2 = vals[3];
  return true;
}

// 解析 [[x,y], [x,y], ...] 为多边形点列表
bool parse_polygon(const std::string& s, std::vector<Point>& out) {
  std::string t = s;
  // 找到最外层 [ ]
  size_t lb = t.find("[[");
  size_t rb = t.rfind("]]");
  if (lb == std::string::npos || rb == std::string::npos) return false;
  t = t.substr(lb + 1, rb - lb + 1);  // 包含外层 [ ]
  // 逐个提取 [x, y]
  size_t pos = 0;
  while (true) {
    size_t ib = t.find('[', pos);
    if (ib == std::string::npos) break;
    size_t ie = t.find(']', ib);
    if (ie == std::string::npos) break;
    std::string pair = t.substr(ib + 1, ie - ib - 1);
    std::stringstream ss(pair);
    std::string item;
    std::vector<double> vals;
    while (std::getline(ss, item, ',')) {
      try {
        vals.push_back(std::stod(trim(item)));
      } catch (...) {
        return false;
      }
    }
    if (vals.size() != 2) return false;
    out.push_back({vals[0], vals[1]});
    pos = ie + 1;
  }
  return !out.empty();
}

}  // namespace

bool parse_stream_profile(const std::string& yaml_content,
                          const std::string& target_id,
                          StreamProfile& out, std::string& err) {
  out = StreamProfile{};
  out.stream_id = target_id;

  std::stringstream ss(yaml_content);
  std::string line;
  bool in_streams = false;
  bool in_target = false;
  int target_indent = -1;
  bool in_rects = false;
  bool in_polys = false;
  int list_indent = -1;

  while (std::getline(ss, line)) {
    // 计算缩进
    int indent = 0;
    while (indent < static_cast<int>(line.size()) &&
           line[indent] == ' ') ++indent;
    std::string content = trim(line);
    if (content.empty() || content[0] == '#') continue;

    // 退出条件：缩进回到 streams 级别或更浅，且不在 streams 内
    if (in_target && indent <= target_indent && !content.empty()) {
      in_target = false;
      in_rects = false;
      in_polys = false;
    }
    if (in_streams && indent == 0) {
      in_streams = false;
    }

    if (content == "streams:") {
      in_streams = true;
      continue;
    }

    if (in_streams && !in_target) {
      // 查找目标 stream_id 的 key（如 "A:" 或 "B:"）
      size_t colon = content.find(':');
      if (colon != std::string::npos) {
        std::string key = trim(content.substr(0, colon));
        if (key == target_id) {
          in_target = true;
          target_indent = indent;
          continue;
        }
      }
    }

    if (!in_target) continue;

    // 在目标流内部：解析字段
    if (in_rects) {
      if (indent <= list_indent) {
        in_rects = false;
      } else {
        // 解析 "- [x1, y1, x2, y2]"
        std::string item = content;
        if (item[0] == '-') item = trim(item.substr(1));
        Rect r;
        if (parse_rect(item, r)) {
          out.forbidden_rectangles.push_back(r);
        }
        continue;
      }
    }
    if (in_polys) {
      if (indent <= list_indent) {
        in_polys = false;
      } else {
        std::string item = content;
        if (item[0] == '-') item = trim(item.substr(1));
        std::vector<Point> poly;
        if (parse_polygon(item, poly)) {
          out.forbidden_polygons.push_back(poly);
        }
        continue;
      }
    }

    size_t colon = content.find(':');
    if (colon == std::string::npos) continue;
    std::string key = trim(content.substr(0, colon));
    std::string val = trim(content.substr(colon + 1));

    if (key == "enabled") {
      out.enabled = (val == "true");
    } else if (key == "input_url") {
      out.input_url = unquote(val);
    } else if (key == "output_url") {
      out.output_url = unquote(val);
    } else if (key == "coordinate_model") {
      out.coordinate_model_path = unquote(val);
    } else if (key == "camera_param") {
      try { out.camera_param = std::stod(val); } catch (...) {}
    } else if (key == "forbidden_rectangles") {
      if (val.empty()) {
        in_rects = true;
        list_indent = indent;
      } else {
        // 单行格式（较少见）
        Rect r;
        if (parse_rect(val, r)) out.forbidden_rectangles.push_back(r);
      }
    } else if (key == "forbidden_polygons") {
      if (val.empty()) {
        in_polys = true;
        list_indent = indent;
      } else {
        std::vector<Point> poly;
        if (parse_polygon(val, poly)) out.forbidden_polygons.push_back(poly);
      }
    }
  }

  if (out.input_url.empty() && out.output_url.empty()) {
    err = "未找到 stream_id=" + target_id + " 的配置";
    return false;
  }
  return true;
}

bool validate_stream_profile(const StreamProfile& p, std::string& err) {
  if (p.stream_id.empty()) { err = "stream_id 为空"; return false; }
  if (p.input_url.empty()) { err = "input_url 为空"; return false; }
  if (p.output_url.empty()) { err = "output_url 为空"; return false; }
  return true;
}

}  // namespace hzw
