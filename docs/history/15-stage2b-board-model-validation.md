# 阶段2B：BM1684 板端 bmodel 真实加载与兼容性验证

> 日期：2026-07-21  
> 环境：BM1684 工控机（libsophon 0.4.9 LTS）  
> 分支：`feat/bm1684-edge-deployment`  
> Git commit：`9255f27`（feat: 提交BM1684 F32部署模型）

## 1. 概要

在 BM1684 板端使用 BMRuntime 对 x86 端转换的 F32 bmodel 进行真实加载验证，确认模型文件完整、兼容性无问题、网络输入输出符合预期。

## 2. 模型校验

| 项目 | 值 |
|---|---|
| 文件路径 | `artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel` |
| 文件大小 | 24,428,544 字节 |
| SHA256 | `d1c295c504888541c27e20fda500976725b9d13b6ef5c91842f247f52c31cfcd` |
| Git 提交 | `9255f27`（由 `git ls-files` 确认已跟踪） |

## 3. 系统资源

| 项目 | 值 |
|---|---|
| 芯片 | BM1684-SOC |
| libsophon 版本 | 0.4.9 LTS（库版本与驱动版本一致） |
| TPU 内存 | 550M（基线 75M 已用） |
| 主机内存 | 2.6Gi（可用 1.7Gi） |
| 磁盘 | 1.5G 可用（overlay） |

## 4. bmrt_test 真实加载

```bash
/opt/sophon/libsophon-current/bin/bmrt_test \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --devid 0
```

**结果：退出码 0**

| 项目 | 值 |
|---|---|
| 网络名称 | `yolov7_ship` |
| 网络数量 | 1 |
| 输入名称 | `images` |
| 输入 Shape | `[1, 3, 640, 640]` |
| 输入 dtype | `FLOAT32` |
| 输出名称 | `output_Concat` |
| 输出 Shape | `[1, 25200, 6]` |
| 输出 dtype | `FLOAT32` |
| 推理耗时 | 12,052 us（NPU 11,936 us, CPU 116 us） |
| 兼容性错误 | 无 |
| 内存分配失败 | 无 |
| CPU 算子加载错误 | 无 |

## 5. bmrt_load_test 编译与运行

```bash
g++ -std=c++14 -I/opt/sophon/libsophon-0.4.9/include \
    tools/image_inference/bmrt_load_test.cpp -o tools/image_inference/bmrt_load_test \
    -L/opt/sophon/libsophon-0.4.9/lib -lbmrt -lbmlib -lpthread -ldl

LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib \
  ./tools/image_inference/bmrt_load_test \
  artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel 0
```

**结果：退出码 0**

| 项目 | 值 |
|---|---|
| BM 设备 0 打开 | 成功 |
| bmodel 加载 | 成功 |
| 网络数 | 1 |
| 网络类型 | 静态 |
| 输入名 | `images`（动态读取） |
| 输入类型 | `FLOAT32` |
| 输入 Shape | `[1, 3, 640, 640]` |
| 输出名 | `output_Concat`（动态读取，非硬编码 `output`） |
| 输出类型 | `FLOAT32` |
| 输出 Shape | `[1, 25200, 6]` |
| 资源释放 | 正常（TPU 无残留进程） |

## 6. 门禁判定

| # | 条件 | 状态 |
|---|---|---|
| 1 | Git 成功同步至包含 `9255f27` 的版本 | ✅ |
| 2 | bmodel 文件大小正确（24,428,544 字节） | ✅ |
| 3 | SHA256 完全一致 | ✅ |
| 4 | `bmrt_test` 真实加载成功（退出码 0） | ✅ |
| 5 | `bmrt_load_test` 真实加载成功（退出码 0） | ✅ |
| 6 | 输入为 `images [1,3,640,640] FLOAT32` | ✅ |
| 7 | 输出为 `[1,25200,6] FLOAT32`（实际输出名 `output_Concat`） | ✅ |
| 8 | 无运行时兼容错误 | ✅ |
| 9 | 测试结束后 TPU 资源正常释放 | ✅ |
| 10 | 未修改 libsophon 和系统服务 | ✅ |

**阶段2 最终门禁：✅ 通过。阶段2B 闭合，可进入阶段3。**

## 7. 注意事项

- 未安装 TPU-MLIR、Python SAIL、Torch、CUDA 或 onnxruntime-gpu
- 未修改板端 libsophon
- 未执行 INT8 转换
- 所有日志和报告使用中文
