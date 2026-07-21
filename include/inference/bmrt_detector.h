// -*- coding: utf-8 -*-
// =============================================================================
// BmrtDetector：BM1684 BMRuntime 推理封装（阶段3 单图 PoC，可复用于后续视频 Pipeline）。
//
// 生命周期约定（与阶段3 要求一致）：
//   - 构造时一次性：打开设备 -> 创建 BMRuntime -> 加载 bmodel -> 读取网络信息；
//   - infer() 每次推理复用已加载模型，禁止重复加载；
//   - 析构时统一释放资源。
//
// 网络输入输出名称/形状/dtype 全部从 bm_net_info_t 动态读取，绝不硬编码 "output"。
// =============================================================================
#ifndef HZW_INFERENCE_BMRT_DETECTOR_H
#define HZW_INFERENCE_BMRT_DETECTOR_H

#include <string>
#include <vector>

namespace hzw {

class BmrtDetector {
 public:
  // dev_id：BM 设备号；bmodel_path：bmodel 文件路径。
  BmrtDetector(int dev_id, const std::string& bmodel_path);
  ~BmrtDetector();

  BmrtDetector(const BmrtDetector&) = delete;
  BmrtDetector& operator=(const BmrtDetector&) = delete;

  // 是否成功初始化（设备打开 + bmodel 加载 + 网络信息校验通过）。
  bool ok() const;
  std::string last_error() const;

  // 网络信息（动态读取）。
  std::string net_name() const;
  std::string input_name() const;     // 预期 "images"
  std::string output_name() const;    // 预期 "output_Concat"（以板端实际为准）
  std::vector<int> input_shape() const;   // 预期 [1,3,640,640]
  std::vector<int> output_shape() const;  // 预期 [1,25200,6]
  int input_element_count() const;    // 输入元素个数（= 1*3*640*640）
  int output_element_count() const;   // 输出元素个数（= 1*25200*6）

  // 推理：input 为 NCHW FLOAT32（长度 = input_element_count()），
  //       输出写入 output（长度 = output_element_count()）。
  // 返回 false 时 last_error() 给出原因。
  bool infer(const std::vector<float>& input, std::vector<float>& output);

  // 返回已打开的 BM 设备句柄（bm_handle_t，以 void* 暴露避免头文件耦合）。
  // 用于复用同一设备做 BMCV 预处理/绘制；未就绪时返回 nullptr。
  void* handle() const;

 private:
  struct Impl;
  Impl* p_;
};

}  // namespace hzw

#endif  // HZW_INFERENCE_BMRT_DETECTOR_H
