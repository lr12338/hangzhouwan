# 阶段3：BM1684 单图 C++ 推理、YOLOv7 后处理与绘框 PoC

> 日期：2026-07-21  
> 环境：BM1684 工控机（libsophon 0.4.9 LTS）  
> 分支：`feat/bm1684-edge-deployment`  
> Git commit：`9255f27`（模型基线上）  
> 模型：`artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel` (F32, 24428544 字节)

## 1. 概要

在 BM1684 板端基于 `libsophon` BMRuntime C API 实现单图推理、YOLOv7 后处理和绘框 PoC，链路为：

```
JPEG 图片 → libjpeg 解码(RGB) → 直接 resize 640×640 → /255 → NCHW FLOAT32
→ BMRuntime 推理 → [1,25200,6] → 置信度过滤 → 坐标换算 → NMS
→ 绘制检测框 → 输出 JPEG + JSON
```

## 2. 代码结构

```
include/
├── image_io/jpeg_io.h           # RGB Image 结构, JPEG 读写, resize, 绘框, 绘字
├── inference/bmrt_detector.h    # BmrtDetector（pImpl，动态读取模型信息）
├── inference/yolov7_postprocess.h # YOLOv7 后处理（纯函数，可独立测试）
└── util/sha256.h                # SHA-256（JSON model_sha256 字段验证）

src/
├── image_io/jpeg_io.cpp         # libjpeg 解码/编码, 双线性 resize, 5x7 位图字体
├── inference/bmrt_detector.cpp  # BmrtDetector 实现（设备->bmrt->加载->推理生命周期）
├── inference/yolov7_postprocess.cpp # 后处理: obj/score 过滤, 坐标映射, 贪心 NMS
└── util/sha256.cpp              # 标准 SHA-256

tools/image_inference/
├── single_image_infer.cpp       # 单图推理工具（含 --loop N 重复推理模式）
├── bmrt_load_test.cpp           # 阶段2 门禁：最小加载验证
└── CMakeLists.txt               # （根目录 CMakeLists.txt 统一构建）

tests/unit_cpp/
└── test_yolov7_postprocess.cpp  # 后处理单元测试（10 项测试用例）
```

## 3. 技术要点

### 3.1 预处理（与原 Python 契约一致）

| 步骤 | 实现 | 与 Python cv2 等 |
|---|---|---|
| JPEG 解码 | `libjpeg` JCS_RGB | 等价于 cv2.imread(BGR) + cvtColor(BGR2RGB) |
| 缩放 | 双线性（半像素中心对齐） | 贴近 cv2.resize INTER_LINEAR |
| 输入尺寸 | 直接 640×640（无 letterbox） | 一致 |
| 归一化 | 除以 255.0 | 一致 |
| 布局 | HWC → CHW, NCHW FLOAT32 | 一致 |

### 3.2 后处理（与 detector.py 语义一致）

| 步骤 | 实现 |
|---|---|
| 输出解析 | 输出名 `output_Concat` 从 `bm_net_info_t` 动态读取，未硬编码 |
| 每行 6 值 | cx, cy, w, h, object_confidence, class_confidence |
| 置信度过滤 | object_confidence > conf_threshold; score = obj_conf × class_conf > conf_threshold |
| 坐标映射 | 640 空间 → 原图：scale_x = orig_w/640, scale_y = orig_h/640；xyxy 坐标 |
| 裁剪 | 裁剪到图像边界，剔除 NaN/Inf/负宽高 |
| NMS | 单类别贪心 NMS（xyxy IoU），按 score 降序，最大候选数保护（5000） |

### 3.3 BMRuntime 生命周期

- 构造一次：打开设备 → 创建 BMRuntime → 加载 bmodel → 读取网络信息
- `infer()` 每次推理复用模型，不重复加载
- 析构统一释放
- 使用 `bmrt_tensor` 分配设备内存，`bmrt_launch_tensor_ex` 推理，`bm_thread_sync` 同步

## 4. 测试结果

### 4.1 单图推理（frame_30.jpg, 960×544）

| 指标 | 值 |
|---|---|
| 输入 | `images [1,3,640,640] FLOAT32` |
| 输出 | `output_Concat [1,25200,6] FLOAT32` |
| 预处理耗时 | 56.3 ms |
| 推理耗时 | 16.3 ms |
| 后处理耗时 | 0.6 ms |
| 检测数量 | 2（conf=0.1, iou=0.1） |
| 检测详情 | ship 0.257 (329,313,371,339); ship 0.126 (563,234,592,238) |

### 4.2 空检测（frame_000010.jpg）

| 指标 | 值 |
|---|---|
| 检测数量 | 0（该帧预估无船） |
| max object_confidence | 0.0099（远低于阈值 0.1） |

### 4.3 100 次重复推理

| 指标 | 值 |
|---|---|
| 平均 | 15.99 ms |
| P50 | 15.95 ms |
| P95 | 16.55 ms |
| 最大 | 16.55 ms |
| 检测框数稳定性 | 稳定（100 次均为 2） |
| 主机内存 | 前后无变化（850Mi used） |
| TPU 状态 | 无残留进程（75M/550M） |

### 4.4 错误场景

| 场景 | 退出码 | 错误信息 |
|---|---|---|
| 模型文件不存在 | 255（BMRT FATAL） | File[..] open failed |
| 图片文件不存在 | 4 | 错误 \| 图片解码 |
| 错误设备号 | 3 | 打开 BM 设备 99 失败 |
| 输出目录不存在 | 0（自动创建） | 正常输出 |

### 4.5 单元测试

10 项 YOLOv7 后处理测试全部通过：
- 单框检测、obj_conf 过滤、score 过滤、NMS 抑制重叠框、NMS 保留非重叠框、零输出、边界裁剪、无效输入、候选数上限、IoU 辅助函数

### 4.6 Python 测试

运行 `python3 tests/run_tests.py`：26 项通过，2 项跳过（ONNX 审计，因 best.onnx 被 gitignore）

## 5. 精度声明

当前仓库无帧 30 的 x86 ONNX 基线检测 JSON。板端 F32 单图推理、后处理和绘框功能已通过；**最终检测框精度仍需与同图 ONNX 基线 JSON 对照**。

## 6. 未完成事项

- [ ] 与 x86 ONNX 基线 JSON 进行精度对照（IoU、score 差异、漏检/误检）
- [ ] 视频流管线（RTSP → 连续推理 → RTMP，阶段4）
- [ ] INT8 量化（阶段后续）
- [ ] AIS 坐标映射（阶段5）

## 7. 结论

**板端 F32 单图 C++ 推理功能通过，精度对照待 x86 基线。**（对应目标结论 3）

阶段2B 和阶段3 的 PoC 开发已完成，可以进入阶段4（视频管线）。
