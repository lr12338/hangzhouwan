# 迁移进度跟踪（PROGRESS）

> 本文件是**当前测试进展的唯一权威索引**。每完成一个阶段或关键决策，更新本文件。新开发者先读 [../README.md](../README.md)，再读本文件，再按 [00-index.md](00-index.md) 深入。

| 项 | 值 |
|---|---|
| 最后更新 | 2026-07-21（阶段4 单路硬件视频管线 PoC 功能通过，2h 门禁待执行） |
| 仓库 | `https://github.com/lr12338/hangzhouwan.git` |
| 本地路径 | `/home/linaro/hangzhouwan-orign/hangzhouwan` |
| 当前分支 | `feat/bm1684-edge-deployment` |
| HEAD | `8e3217e` + 阶段4 未提交修改 |
| 当前阶段 | **阶段4 单路硬件视频管线 PoC 功能通过，2h 门禁待执行** |
| 总体健康度 | 🟢 阶段3 闭合，阶段4 功能通过/2h 待执行 |

---

## 1. 阶段总览

| # | 阶段 | 状态 | 门禁 | 备注 |
|---|---|---|---|---|
| 0 | 项目与设备基线确认 | ✅ 完成 | 通过 | 三份文档 + chip 探针已交付 |
| 1 | 测试基线 + 安全配置 | ✅ 完成 | 源码可复现、离线测试通过 | 26 项测试通过；源码已脱敏 |
| 2 | ONNX 审计 + bmodel 转换 | ✅ 完成 | x86 F32 转换和数值验证通过 | `9255f27` 提交，bmodel 已跟踪 |
| 2B | 板端 bmodel 加载与兼容性验证 | ✅ 完成 | bmrt_test + bmrt_load_test 通过 | F32 bmodel SHA256 一致，板端真实加载正常 |
| 3 | 单图 C++ 推理 PoC | ✅ 完成 | 单图 PoC 通过 | 精度对照待 x86 基线 |
| 4 | 单路硬件视频管线 PoC | 🔶 功能通过/2h 门禁待执行 | 单路稳定 2h | 5min 测试已过，2h 待环境 |
| 5 | 坐标映射 + AIS 迁移 | ⏳ | 关联不低于旧版 | 待阶段4 |
| 6 | 双路 Pipeline 整合 | ⏳ | 双路运行 | 待阶段5 |
| 7 | 生产部署 + 无人值守 | ⏳ | 部署就绪 | 待阶段6 |
| 8 | 分级稳定性验证 | ⏳ | 验收线达标 | 待阶段7 |
| 9 | 代码收口 + GitHub 交付 | ⏳ | PR 合并 | 待阶段8 |

图例：✅ 完成 · 🔶 进行中/部分完成 · ⛔ 阻塞 · ⏳ 待启动/待前置

---

## 2. 阶段2：ONNX 审计 + bmodel 转换（✅ 完成）

- [x] ONNX 审计（`tools/inspect_onnx.py`）：输入 `images` FLOAT 动态，需固化 `[1,3,640,640]`；输出 `output` 不含 NMS
- [x] INT8 校准集脚本（`tools/convert_model/extract_calibration.sh`）
- [x] x86 转换脚本（`tools/convert_model/convert_bmodel.sh`）
- [x] F32 bmodel 生成：`artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel`（24,428,544 字节）
- [x] SHA256：`d1c295c504888541c27e20fda500976725b9d13b6ef5c91842f247f52c31cfcd`
- [x] ONNX/MLIR 172 个 Tensor 数值验证通过；bmodel cmodel 比较通过
- [x] 模型提交：`9255f27`（bmodel 已 Git 跟踪，`git ls-files` 确认）
- [x] 文档：`docs/06-stage2-onnx-audit-and-bmodel.md`、`docs/12`、`docs/13`、`docs/14`

**阶段2 门禁：✅ 通过**

---

## 3. 阶段2B：板端 bmodel 真实加载（✅ 完成）

参见 `docs/15-stage2b-board-model-validation.md`。

- [x] bmrt_test 真实加载成功（退出码 0）
- [x] bmrt_load_test 编译运行成功（退出码 0）
- [x] 网络 `yolov7_ship`：输入 `images [1,3,640,640] FLOAT32`，输出 `output_Concat [1,25200,6] FLOAT32`
- [x] 无兼容性错误、无内存分配失败、TPU 资源正常释放
- [x] 未修改 libsophon 和系统服务

**阶段2B 门禁：✅ 通过**

---

## 4. 阶段3：单图 C++ 推理 PoC（✅ 完成）

参见 `docs/16-stage3-single-image-cpp-poc.md`。

### 新增代码

| 模块 | 文件 |
|---|---|
| 图像 I/O | `include/image_io/jpeg_io.h`, `src/image_io/jpeg_io.cpp` |
| 推理封装 | `include/inference/bmrt_detector.h`, `src/inference/bmrt_detector.cpp` |
| 后处理 | `include/inference/yolov7_postprocess.h`, `src/inference/yolov7_postprocess.cpp` |
| SHA-256 | `include/util/sha256.h`, `src/util/sha256.cpp` |
| 单图工具 | `tools/image_inference/single_image_infer.cpp` |
| 单元测试 | `tests/unit_cpp/test_yolov7_postprocess.cpp` |
| 构建 | `CMakeLists.txt`（根目录） |

### 测试结果

| 测试 | 结果 |
|---|---|
| 单元测试（10 项） | ✅ 全部通过 |
| 单图推理（frame_30.jpg） | ✅ 2 个检测，预处理 56ms，推理 16ms，后处理 0.6ms |
| 空检测（frame_000010.jpg） | ✅ 0 检测（该帧无船） |
| 100 次重复推理 | ✅ 平均 15.99ms，框数稳定，无内存泄漏 |
| 错误场景（3 项） | ✅ 正确错误码和错误信息 |
| 输出目录自动创建 | ✅ |
| JSON 可解析 | ✅ |
| Python 测试（26 项） | ✅ 全部通过 |

### 精度

精度对照待 x86 基线（当前无同图 ONNX 检测 JSON）。板端 F32 推理功能已通过。

---

## 5. 当前阻塞与待办

### 待办

- [ ] 与 x86 ONNX 基线 JSON 进行精度对照（IoU、score 差异）
- [ ] 进入阶段4：单路硬件视频管线 PoC
- [ ] 后续阶段：INT8 量化、AIS 坐标映射、双路 Pipeline、部署

> 阶段1 的 B1/B2（三模型缺失）已于阶段2 解除。
> 阶段2 的历史阻塞（`weights/best.onnx` 缺失、bmodel 未生成、无测试图片）均已解除。

---

## 5. 阶段4：单路硬件视频管线 PoC（🔶 功能通过，2h 门禁待执行）

参见 `docs/17-stage4-single-video-hardware-pipeline.md` 与 `docs/18-stage4-stability-test.md`。

### 新增代码

| 模块 | 文件 |
|---|---|
| 视频源（h264_bm 解码） | `include/video/video_source.h`, `src/video/sophon_ffmpeg_source.cpp` |
| 视频输出（h264_bm 编码+MP4/TS 封装） | `include/video/video_sink.h`, `src/video/sophon_ffmpeg_sink.cpp` |
| 最新帧队列（容量1，丢旧帧） | `include/video/latest_frame_queue.h` |
| 视频帧结构 | `include/video/video_frame.h` |
| FFmpeg C++ 兼容（extern "C"） | `include/video/ffmpeg_compat.h` |
| 检测快照（TTL 复用） | `include/pipeline/detection_snapshot.h` |
| 单路管线（三线程） | `include/pipeline/single_stream_pipeline.h`, `src/pipeline/single_stream_pipeline.cpp` |
| 指标统计 | `include/monitoring/pipeline_metrics.h`, `src/monitoring/pipeline_metrics.cpp` |
| CLI 工具 | `tools/video_inference/single_video_infer.cpp` |
| 稳定性测试脚本 | `tools/video_inference/stability_test.sh` |
| 单元测试 | `tests/unit_cpp/test_latest_frame_queue.cpp`, `test_detection_snapshot.cpp`, `test_pipeline_timing.cpp` |
| 文档 | `docs/17-stage4-single-video-hardware-pipeline.md`, `docs/18-stage4-stability-test.md` |

### 核心结论

- **硬件解码**：Sophon-FFmpeg C API `h264_bm`，输出 NV12（host 可读写 mmap'd bm_image）
- **硬件编码**：Sophon-FFmpeg C API `h264_bm`，接受解码 AVFrame（bm_image），in-place 像素修改对编码器可见
- **解码缓冲池**：`extra_frame_buffer_num=20`（经 AVDictionary 传入），避免编码器 11 帧保留导致的 bm_image 池死锁
- **预处理优化**：用 `sws_scale` 一次完成 NV12 960×544 → RGB 640×640（SIMD），绕开 CPU `resize_bilinear` 瓶颈（54.6ms → 22.3ms）
- **队列策略**：`LatestFrameQueue<VideoFrame>` 模板，容量1，`push` 满时丢最旧帧并 release（归还 bm_image）
- **检测复用**：`DetectionSnapshot` + TTL（1000ms），非推理帧复用最近结果绘框
- **响应信号**：`SIGINT`/`SIGTERM` 优雅停止，关闭队列并释放全部资源

### 测试结果

| 测试 | 结果 |
|---|---|
| 单元测试（4 项：队列/快照/管线调度/后处理） | ✅ 全部通过 |
| 单图基线回归（10 项） | ✅ 全部通过 |
| Python 测试（26 项） | ✅ 全部通过 |
| 10s 短跑（h264_bm 解码+编码+推理+绘框） | ✅ 退出码 0，输出可解码，含检测框 |
| 5min 冒烟测试 | ✅ 退出码 0，队列≤1，延迟 P95 < 500ms，无内存泄漏 |
| 30min 中等测试 | ⏳ 可执行（`./stability_test.sh 30`） |
| 2h 最终门禁 | ⏳ 提供可执行脚本（`./stability_test.sh 120`），未真实执行 |

### 已知限制

- **CPU 预处理瓶颈**：当前 sws_scale NV12→RGB 640×640 + NCHW 约 22ms/帧，加上 sws_scale 960×544 转换与绘框开销，总处理约 200ms/帧，制约输出帧率至约 5fps（低于 10fps 目标）。阶段4.1 优化方向：BMCV/VPP 硬件预处理（NV12 960×544 → 640×640 → BGR/NCHW 直连设备内存）。
- **解码器警告**：`pkt can't be sent to decoder` 出现在循环 seek 期间，为 BM 视频解码器内部缓冲池回收提示，不影响功能。

### 回滚方法

- 阶段4 所有新增 C++ 源码位于 `include/video/`、`include/pipeline/`、`include/monitoring/`、`src/video/`、`src/pipeline/`、`src/monitoring/`、`tools/video_inference/`、`tests/unit_cpp/test_*.cpp`（除 `test_yolov7_postprocess.cpp`）。
- 不修改 `src/inference/`、`src/image_io/`、`src/util/` 与 `tools/image_inference/`。
- CMakeLists.txt 增量添加，可独立 revert。
- `docs/17`、`docs/18` 独立文档。
- 回滚单次提交即可恢复至阶段3 基线。
