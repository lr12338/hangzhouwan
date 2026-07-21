# 板端 BMRuntime 推理工具（BM1684）

`bmrt_load_test.cpp` 是阶段2 门禁"最小 C++ 加载成功"的最小程序。`single_image_infer.cpp` 是阶段3 单图推理 PoC。

## 构建（CMake）

```bash
# 根目录
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

产出：
- `build/bmrt_load_test` — 阶段2 门禁：最小加载验证
- `build/single_image_infer` — 阶段3 PoC：单图推理 + 后处理 + 绘框
- `build/test_postprocess` — 后处理单元测试

## 手动编译（单文件）

```bash
# bmrt_load_test（阶段2）
g++ -std=c++14 -I/opt/sophon/libsophon-0.4.9/include \
    tools/image_inference/bmrt_load_test.cpp -o tools/image_inference/bmrt_load_test \
    -L/opt/sophon/libsophon-0.4.9/lib -lbmrt -lbmlib -lpthread -ldl
```

> 编译产物已 gitignore；仅提交源码。

## 运行

```bash
export LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib

# 阶段2 门禁：加载 bmodel 并打印网络信息
./build/bmrt_load_test artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel 0

# 阶段3 单图推理
./build/single_image_infer \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --image testdata/model_test/frame_30.jpg \
  --output result.jpg --json result.json \
  --conf 0.1 --iou 0.1 --device 0

# 100 次重复推理（统计耗时）
./build/single_image_infer \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --image testdata/model_test/frame_30.jpg \
  --conf 0.1 --iou 0.1 --device 0 --loop 100
```

## 已验证（阶段2B + 阶段3，工控机）

- **bmrt_load_test**：BM 设备 0 打开成功，bmodel 加载成功，网络静态，输入 `images [1,3,640,640] FLOAT32`，输出 `output_Concat [1,25200,6] FLOAT32`，资源释放正常，退出码 0
- **single_image_infer**：JPEG → 推理 → 检测 → 绘框 → JSON 全链路通过；预处理 56ms，推理 16ms，后处理 0.6ms；100 次重复推理平均 15.99ms，框数稳定，无内存泄漏
- 输出名 `output_Concat` 从 `bm_net_info_t` 动态读取，未硬编码 `output`
