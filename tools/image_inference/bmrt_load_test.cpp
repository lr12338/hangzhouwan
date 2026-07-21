// -*- coding: utf-8 -*-
// =============================================================================
// 最小 BMRuntime bmodel 加载验证程序（阶段2 门禁：最小 C++ 加载成功）。
//
// 用途：在 BM1684 板端加载 x86 转换得到的 bmodel，打印网络输入/输出形状与类型，
//       验证 bmodel 可被 BMRuntime 正常解析（精度/推理验证见阶段3 单图 PoC）。
//
// 编译（工控机）：
//   g++ -std=c++14 -I/opt/sophon/libsophon-0.4.9/include \
//       tools/image_inference/bmrt_load_test.cpp -o tools/image_inference/bmrt_load_test \
//       -L/opt/sophon/libsophon-0.4.9/lib -lbmrt -lbmlib -lpthread -ldl
//
// 运行：
//   ./tools/image_inference/bmrt_load_test weights/yolov7_ship_1684_f32.bmodel
//   ./tools/image_inference/bmrt_load_test            # 无 bmodel，仅验证运行时链路
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "bmruntime_interface.h"
#include "bmlib_runtime.h"

static const char* dtype_name(bm_data_type_t t) {
  switch (t) {
    case BM_FLOAT32: return "FLOAT32";
    case BM_FLOAT16: return "FLOAT16";
    case BM_INT8:    return "INT8";
    case BM_UINT8:   return "UINT8";
    case BM_INT16:   return "INT16";
    case BM_INT32:   return "INT32";
    default:         return "OTHER";
  }
}

static void print_shape(const bm_shape_t& s) {
  printf("[");
  for (int i = 0; i < s.num_dims; ++i) {
    printf("%d%s", s.dims[i], (i + 1 < s.num_dims) ? "," : "");
  }
  printf("]");
}

int main(int argc, char** argv) {
  const char* bmodel_path = (argc >= 2) ? argv[1] : nullptr;
  int dev_id = (argc >= 3) ? std::atoi(argv[2]) : 0;

  bm_handle_t handle = nullptr;
  bm_status_t ret = bm_dev_request(&handle, dev_id);
  if (ret != 0 || handle == nullptr) {
    printf("错误 | BMRuntime加载 | 打开 BM 设备 %d 失败 ret=%d\n", dev_id, ret);
    return 1;
  }
  printf("信息 | BMRuntime加载 | 已打开 BM 设备 %d\n", dev_id);

  void* bmrt = bmrt_create(handle);
  if (bmrt == nullptr) {
    printf("错误 | BMRuntime加载 | bmrt_create 失败\n");
    bm_dev_free(handle);
    return 1;
  }

  if (bmodel_path == nullptr) {
    printf("信息 | BMRuntime加载 | 未提供 bmodel，运行时链路正常（仅验证 API 链接）\n");
    printf("用法 | %s <bmodel路径> [设备号]\n", argv[0]);
    bmrt_destroy(bmrt);
    bm_dev_free(handle);
    return 0;
  }

  printf("信息 | BMRuntime加载 | 正在加载 bmodel: %s\n", bmodel_path);
  bool ok = bmrt_load_bmodel(bmrt, bmodel_path);
  if (!ok) {
    printf("错误 | BMRuntime加载 | 加载 bmodel 失败: %s\n", bmodel_path);
    bmrt_destroy(bmrt);
    bm_dev_free(handle);
    return 1;
  }

  int net_num = bmrt_get_network_number(bmrt);
  printf("信息 | BMRuntime加载 | 加载成功，网络数=%d\n", net_num);

  const char** net_names = nullptr;
  bmrt_get_network_names(bmrt, &net_names);
  for (int n = 0; n < net_num; ++n) {
    const char* name = net_names[n];
    const bm_net_info_t* info = bmrt_get_network_info(bmrt, name);
    if (info == nullptr) {
      printf("警告 | BMRuntime加载 | 网络 %s 无法获取信息\n", name);
      continue;
    }
    printf("---- 网络[%d]: %s （%s）----\n", n, name,
           info->is_dynamic ? "动态" : "静态");
    int st = (info->stage_num > 0) ? 0 : -1;
    for (int i = 0; i < info->input_num; ++i) {
      printf("  输入 %s : %s ", info->input_names[i], dtype_name(info->input_dtypes[i]));
      if (st >= 0) print_shape(info->stages[st].input_shapes[i]);
      printf("\n");
    }
    for (int o = 0; o < info->output_num; ++o) {
      printf("  输出 %s : %s ", info->output_names[o], dtype_name(info->output_dtypes[o]));
      if (st >= 0) print_shape(info->stages[st].output_shapes[o]);
      printf("\n");
    }
  }

  bmrt_destroy(bmrt);
  bm_dev_free(handle);
  printf("信息 | BMRuntime加载 | 资源已释放，验证通过\n");
  return 0;
}
