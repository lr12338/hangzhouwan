# 迁移进度跟踪（PROGRESS）

> 本文件是**当前测试进展的唯一权威索引**。每完成一个阶段或关键决策，更新本文件。新开发者先读 [../README.md](../README.md)，再读本文件，再按 [00-index.md](00-index.md) 深入。

| 项 | 值 |
|---|---|
| 最后更新 | 2026-07-21（阶段4.2 单路 RTSP 输入代码+单元测试+连接路径验证完成；实流/长时待人工；30min/2h 门禁待人工执行） |
| 仓库 | `https://github.com/lr12338/hangzhouwan.git` |
| 本地路径 | `/home/linaro/hangzhouwan-orign/hangzhouwan` |
| 当前分支 | `feat/bm1684-edge-deployment` |
| HEAD | 阶段4.2 已提交（feat `e010a98` + docs）；工作区干净 |
| 当前阶段（4.2） | **阶段4 本地视频功能+300s 验证通过；阶段4.2 单路 RTSP 输入代码+单元测试+连接路径验证通过；实流与 30min/2h 门禁待人工执行** |
| 总体健康度 | 🟢 阶段3 闭合，阶段4 功能通过+300s+10fps；阶段4.2 RTSP 代码+单测+连接路径验证通过，实流待人工 |

---

## 1. 阶段总览

| # | 阶段 | 状态 | 门禁 | 备注 |
|---|---|---|---|---|
| 0 | 项目与设备基线确认 | ✅ 完成 | 通过 | 三份文档 + chip 探针已交付 |
| 1 | 测试基线 + 安全配置 | ✅ 完成 | 源码可复现、离线测试通过 | 26 项测试通过；源码已脱敏 |
| 2 | ONNX 审计 + bmodel 转换 | ✅ 完成 | x86 F32 转换和数值验证通过 | `9255f27` 提交，bmodel 已跟踪 |
| 2B | 板端 bmodel 加载与兼容性验证 | ✅ 完成 | bmrt_test + bmrt_load_test 通过 | F32 bmodel SHA256 一致，板端真实加载正常 |
| 3 | 单图 C++ 推理 PoC | ✅ 完成 | 单图 PoC 通过 | 精度对照待 x86 基线 |
| 4 | 单路硬件视频管线 PoC | 🔶 功能通过+300s验证通过/30min·2h待人工 | 单路稳定 2h | BMCV 绘制优化达 10fps，300s 通过；30min/2h 待人工执行 |
| 4.2 | 单路 RTSP 输入到本地文件 | 🔶 代码+单测+连接路径验证通过/实流·长时待人工 | RTSP 实流 2h | 连接超时/重连/脱敏/PTS 单调；实流解码与中途重连、30min/2h 待人工 |
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
| Python 测试（29 项） | ✅ 全部通过 |

### 精度

精度对照待 x86 基线（当前无同图 ONNX 检测 JSON）。板端 F32 推理功能已通过。

---

## 5. 当前阻塞与待办

### 待办

- [ ] 与 x86 ONNX 基线 JSON 进行精度对照（IoU、score 差异）
- [x] 阶段4：单路硬件视频管线 PoC（功能通过）
- [x] 阶段4 BMCV 性能优化（10fps 达标，300s 验证通过）
- [ ] 阶段4：30min/2h 长时稳定性门禁人工执行
- [x] 阶段4.2：单路 RTSP 输入（连接/读取超时、受控重连、URL 脱敏、env 输入、PTS 单调）代码+单元测试
- [x] 阶段4.2：受控无效地址连接/超时/中断/脱敏路径验证
- [ ] 阶段4.2：RTSP 实流解码 + 中途断线重连人工验证（见 docs/22）
- [ ] 阶段4.2：30min/2h RTSP 长时门禁人工执行
- [ ] 后续阶段：INT8 量化、AIS 坐标映射、双路 Pipeline、部署

> 阶段1 的 B1/B2（三模型缺失）已于阶段2 解除。
> 阶段2 的历史阻塞（`weights/best.onnx` 缺失、bmodel 未生成、无测试图片）均已解除。

---

## 5. 阶段4：单路硬件视频管线 PoC（🔶 功能通过+300s验证通过，30min/2h 待人工执行）

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
| 单元测试（6 项：队列/快照/管线调度/后处理/RTSP 选项/输出 PTS） | ✅ 全部通过 |
| 单图基线回归（10 项） | ✅ 全部通过 |
| Python 测试（29 项） | ✅ 全部通过 |
| 10s 短跑（h264_bm 解码+编码+推理+绘框） | ✅ 退出码 0，输出可解码，含检测框 |
| 5min 冒烟测试 | ✅ 退出码 0，队列≤1，延迟 P95 < 500ms，无内存泄漏 |
| 30min 中等测试 | ⏳ 待人工执行（15min 稳态分析仅为历史参考，不替代完整 1800s 门禁；详见 docs/18、docs/20） |
| 2h 最终门禁 | ⏳ 提供可执行脚本和指南（`docs/20-stage4-manual-long-run-guide.md`），待人工执行 |

### 已知限制

- **BMCV 绘制优化已完成（详见 docs/19）**：原 CPU 路径 sws 往返约 132ms/帧，制约输出至约 5fps；采用 `--draw-mode bmcv`（BMCV draw_rectangle 约 6ms/帧）后，推荐配置 `--preprocess cpu --draw-mode bmcv` 达到 10fps。BMCV VPP resize 在本板不可用，resize 仍由 libswscale 完成；CPU 预处理保留为默认路径（BMCV CSC 与 sws 系数存在差异，检测框 IoU 0.94–0.98 未达门禁）。
- **解码器警告**：`pkt can't be sent to decoder` 出现在循环 seek 期间，为 BM 视频解码器内部缓冲池回收提示，不影响功能。

### 回滚方法

- 阶段4 所有新增 C++ 源码位于 `include/video/`、`include/pipeline/`、`include/monitoring/`、`src/video/`、`src/pipeline/`、`src/monitoring/`、`tools/video_inference/`、`tests/unit_cpp/test_*.cpp`（除 `test_yolov7_postprocess.cpp`）。
- 不修改 `src/inference/`、`src/image_io/`、`src/util/` 与 `tools/image_inference/`。
- CMakeLists.txt 增量添加，可独立 revert。
- `docs/17`、`docs/18` 独立文档。
- 回滚单次提交即可恢复至阶段3 基线。

---

## 6. 阶段4.2：单路 RTSP 输入到本地文件（🔶 代码+单测+连接路径验证通过，实流/长时待人工）

详见 `docs/21-stage4-2-single-rtsp-input.md`、`docs/22-stage4-2-manual-rtsp-stability.md`。

- 扩展现有 `SophonVideoSource`：`open_rtsp`（`rtsp_transport`+`stimeout` 经 AVDictionary 传入，参数来自板端 `ffmpeg -h demuxer=rtsp` 实测）、中断回调（`stop_requested_` 使 open/read 可被信号/限时中断）、`read()` 内受控重连（指数退避，封顶，受 `max_reconnect` 约束）、源 epoch。
- 安全：`--input-env HZW_TEST_RTSP_URL` 环境变量输入，`redact_url_credentials` 脱敏（保留用户名，密码置 `***`），URL 不入命令行/日志/Git。
- 单调 PTS：`OutputPtsSequence` 按输出帧序严格递增，忽略源 PTS；重连后 `SnapshotStore::clear()` 清除过期检测结果。
- 单元测试：`test_rtsp_source_options`（脱敏/env/transport/退避增长·封顶·重置）、`test_output_pts`（严格递增+源 PTS 回退不影响）、`test_detection_snapshot`（clear）。`ctest` 8 项 + Python 29 项全通过。
- 板端验证：20s 文件回归通过（硬件链路未回退）；受控无效地址验证连接超时（`stimeout`）、SIGTERM/SIGINT 中断阻塞（`Immediate exit requested`）、凭据不泄漏、无残留。
- 限制：板端 Sophon-FFmpeg RTSP muxer 不支持 listen，无可用 RTSP 服务端工具，实流解码与中途断线重连待人工用真实摄像头或用户提供的中继执行。
