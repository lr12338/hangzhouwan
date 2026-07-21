# 最小 BMRuntime 加载验证（板端）

`bmrt_load_test.cpp` 是阶段2 门禁“最小 C++ 加载成功”的最小程序：打开 BM 设备、
加载 bmodel、打印网络输入/输出形状与类型，验证 bmodel 可被 BMRuntime 解析。
完整推理 + 后处理 + NMS + 绘框见阶段3 单图 PoC。

## 编译（工控机，C++ 环境已就绪）

```bash
g++ -std=c++14 -I/opt/sophon/libsophon-0.4.9/include \
    tools/image_inference/bmrt_load_test.cpp -o tools/image_inference/bmrt_load_test \
    -L/opt/sophon/libsophon-0.4.9/lib -lbmrt -lbmlib -lpthread -ldl
```

> 编译产物 `bmrt_load_test` 已 gitignore；仅提交源码 `bmrt_load_test.cpp`。

## 运行

```bash
export LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib

# 无 bmodel：验证 BMRuntime API 链路（设备打开 / runtime 创建）
./tools/image_inference/bmrt_load_test

# 有 bmodel（x86 转换后拷贝到 weights/）：加载并打印网络信息
./tools/image_inference/bmrt_load_test weights/yolov7_ship_1684_f32.bmodel 0
```

## 已验证（阶段2，工控机）

- 编译通过（g++ -std=c++14，链接 `libbmrt` + `libbmlib`）；
- 无 bmodel 运行：成功打开 BM 设备 0，BMRuntime CPU op 库加载，API 链路正常；
- 待 x86 转换出 bmodel 后，加载并打印输入/输出形状即满足“最小 C++ 加载成功”门禁。
