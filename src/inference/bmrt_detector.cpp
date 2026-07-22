// -*- coding: utf-8 -*-
#include "inference/bmrt_detector.h"

#include <cstring>
#include <sstream>

extern "C" {
#include "bmlib_runtime.h"
#include "bmruntime_interface.h"
#include "bmruntime_legacy.h"  // bmrt_free_device
#include "bmdef.h"
}

namespace hzw {

struct BmrtDetector::Impl {
  bm_handle_t handle = nullptr;
  void* bmrt = nullptr;
  char** net_names = nullptr;
  int net_num = 0;

  std::string net_name;
  std::string input_name;
  std::string output_name;
  std::vector<int> input_shape;
  std::vector<int> output_shape;
  bm_data_type_t input_dtype = BM_FLOAT32;
  bm_data_type_t output_dtype = BM_FLOAT32;
  bm_shape_t in_shape{};
  bm_shape_t out_shape{};

  bool ready = false;
  std::string error;

  // 预分配的输入/输出设备 Tensor（构造时分配，infer 复用，析构释放）
  bm_tensor_t in_tensor{};
  bm_tensor_t out_tensor{};
  bool tensors_allocated = false;

  void set_error(const std::string& msg) {
    error = msg;
    ready = false;
  }
};

BmrtDetector::BmrtDetector(int dev_id, const std::string& bmodel_path)
    : p_(new Impl()) {
  // 1) 打开设备（仅一次）
  bm_status_t st = bm_dev_request(&p_->handle, dev_id);
  if (st != BM_SUCCESS || p_->handle == nullptr) {
    std::ostringstream os;
    os << "打开 BM 设备 " << dev_id << " 失败 ret=" << st;
    p_->set_error(os.str());
    return;
  }

  // 2) 创建 BMRuntime（仅一次）
  p_->bmrt = bmrt_create(p_->handle);
  if (p_->bmrt == nullptr) {
    p_->set_error("bmrt_create 失败");
    bm_dev_free(p_->handle);
    p_->handle = nullptr;
    return;
  }

  // 3) 加载 bmodel（仅一次）
  if (!bmrt_load_bmodel(p_->bmrt, bmodel_path.c_str())) {
    std::ostringstream os;
    os << "加载 bmodel 失败: " << bmodel_path;
    p_->set_error(os.str());
    return;
  }

  // 4) 动态读取网络信息
  p_->net_num = bmrt_get_network_number(p_->bmrt);
  bmrt_get_network_names(p_->bmrt, const_cast<const char***>(&p_->net_names));
  if (p_->net_num < 1 || p_->net_names == nullptr) {
    p_->set_error("未读取到任何网络");
    return;
  }

  const char* name = p_->net_names[0];
  const bm_net_info_t* info = bmrt_get_network_info(p_->bmrt, name);
  if (info == nullptr) {
    p_->set_error("bmrt_get_network_info 返回空");
    return;
  }
  p_->net_name = info->name;

  if (info->is_dynamic) {
    // 本模型应为静态网络；若动态则记录但不强制失败。
  }
  if (info->input_num < 1 || info->output_num < 1) {
    p_->set_error("网络输入/输出数量异常");
    return;
  }
  if (info->stage_num < 1) {
    p_->set_error("网络 stage_num < 1");
    return;
  }

  p_->input_name = info->input_names[0];
  p_->output_name = info->output_names[0];
  p_->input_dtype = info->input_dtypes[0];
  p_->output_dtype = info->output_dtypes[0];

  const bm_shape_t& ish = info->stages[0].input_shapes[0];
  const bm_shape_t& osh = info->stages[0].output_shapes[0];
  p_->in_shape = ish;
  p_->out_shape = osh;
  for (int i = 0; i < ish.num_dims; ++i) p_->input_shape.push_back(ish.dims[i]);
  for (int i = 0; i < osh.num_dims; ++i) p_->output_shape.push_back(osh.dims[i]);

  // 5) 校验：输入 FLOAT32 [1,3,640,640]，输出 FLOAT32 且元素数 = 25200*6
  if (p_->input_dtype != BM_FLOAT32) {
    p_->set_error("输入 dtype 非 FLOAT32");
    return;
  }
  if (p_->output_dtype != BM_FLOAT32) {
    p_->set_error("输出 dtype 非 FLOAT32");
    return;
  }
  if (input_element_count() != 1 * 3 * 640 * 640) {
    p_->set_error("输入元素数不等于 1*3*640*640");
    return;
  }
  if (output_element_count() != 25200 * 6) {
    p_->set_error("输出元素数不等于 25200*6");
    return;
  }

  p_->ready = true;

  // 预分配输入/输出设备 Tensor（仅一次，后续 infer 复用）
  if (!bmrt_tensor(&p_->in_tensor, p_->bmrt, BM_FLOAT32, p_->in_shape)) {
    p_->set_error("预分配输入 tensor 失败");
    return;
  }
  if (!bmrt_tensor(&p_->out_tensor, p_->bmrt, BM_FLOAT32, p_->out_shape)) {
    p_->set_error("预分配输出 tensor 失败");
    bmrt_free_device(p_->bmrt, p_->in_tensor.device_mem);
    return;
  }
  p_->tensors_allocated = true;
}

BmrtDetector::~BmrtDetector() {
  if (p_) {
    if (p_->tensors_allocated) {
      bmrt_free_device(p_->bmrt, p_->in_tensor.device_mem);
      bmrt_free_device(p_->bmrt, p_->out_tensor.device_mem);
    }
    if (p_->bmrt) bmrt_destroy(p_->bmrt);
    if (p_->handle) bm_dev_free(p_->handle);
    delete p_;
    p_ = nullptr;
  }
}

bool BmrtDetector::ok() const { return p_ && p_->ready; }
std::string BmrtDetector::last_error() const { return p_ ? p_->error : std::string(); }
std::string BmrtDetector::net_name() const { return p_ ? p_->net_name : std::string(); }
std::string BmrtDetector::input_name() const { return p_ ? p_->input_name : std::string(); }
std::string BmrtDetector::output_name() const { return p_ ? p_->output_name : std::string(); }
std::vector<int> BmrtDetector::input_shape() const { return p_ ? p_->input_shape : std::vector<int>(); }
std::vector<int> BmrtDetector::output_shape() const { return p_ ? p_->output_shape : std::vector<int>(); }

int BmrtDetector::input_element_count() const {
  if (!p_) return 0;
  uint64_t n = bmrt_shape_count(&p_->in_shape);
  return static_cast<int>(n);
}
int BmrtDetector::output_element_count() const {
  if (!p_) return 0;
  uint64_t n = bmrt_shape_count(&p_->out_shape);
  return static_cast<int>(n);
}

void* BmrtDetector::handle() const { return (p_ && p_->ready) ? static_cast<void*>(p_->handle) : nullptr; }

bool BmrtDetector::infer(const std::vector<float>& input,
                         std::vector<float>& output) {
  if (!ok()) {
    p_->set_error("探测器未就绪");
    return false;
  }
  if (static_cast<int>(input.size()) != input_element_count()) {
    std::ostringstream os;
    os << "输入长度不匹配: 期望 " << input_element_count()
       << " 实际 " << input.size();
    p_->set_error(os.str());
    return false;
  }

  // 复用预分配的输入/输出设备 Tensor（不再每次 alloc/free）
  bm_tensor_t& in_tensor = p_->in_tensor;
  bm_tensor_t& out_tensor = p_->out_tensor;

  // Host 输入 -> Device
  bm_status_t st = bm_memcpy_s2d(p_->handle, in_tensor.device_mem,
                                 const_cast<float*>(input.data()));
  if (st != BM_SUCCESS) {
    std::ostringstream os;
    os << "bm_memcpy_s2d 失败 ret=" << st;
    p_->set_error(os.str());
    return false;
  }

  // 推理（user_mem=true：使用预分配的输出设备内存）
  bool launched = bmrt_launch_tensor_ex(p_->bmrt, p_->net_name.c_str(),
                                        &in_tensor, 1, &out_tensor, 1,
                                        true, false);
  if (!launched) {
    p_->set_error("bmrt_launch_tensor_ex 失败");
    return false;
  }

  // 等待设备同步
  st = bm_thread_sync(p_->handle);
  if (st != BM_SUCCESS) {
    std::ostringstream os;
    os << "bm_thread_sync 失败 ret=" << st;
    p_->set_error(os.str());
    return false;
  }

  // Device 输出 -> Host
  output.resize(output_element_count());
  st = bm_memcpy_d2s(p_->handle, output.data(), out_tensor.device_mem);
  if (st != BM_SUCCESS) {
    std::ostringstream os;
    os << "bm_memcpy_d2s 失败 ret=" << st;
    p_->set_error(os.str());
    return false;
  }

  return true;
}

}  // namespace hzw
