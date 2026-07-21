# 迁移进度跟踪（PROGRESS）

> 本文件是**当前测试进展的唯一权威索引**。每完成一个阶段或关键决策，更新本文件。新开发者先读 [../README.md](../README.md)，再读本文件，再按 [00-index.md](00-index.md) 深入。

| 项 | 值 |
|---|---|
| 最后更新 | 2026-07-21（阶段2B 板端验证 + 阶段3 单图 C++ PoC 完成） |
| 仓库 | `https://github.com/lr12338/hangzhouwan.git` |
| 本地路径 | `/home/linaro/hangzhouwan-orign/hangzhouwan` |
| 当前分支 | `feat/bm1684-edge-deployment` |
| HEAD | `9255f27`（feat: 提交BM1684 F32部署模型）+ 本地未提交修改 |
| 当前阶段 | **阶段3 单图 C++ 推理 PoC 完成** |
| 总体健康度 | 🟢 阶段2 闭合，阶段3 完成 |

---

## 1. 阶段总览

| # | 阶段 | 状态 | 门禁 | 备注 |
|---|---|---|---|---|
| 0 | 项目与设备基线确认 | ✅ 完成 | 通过 | 三份文档 + chip 探针已交付 |
| 1 | 测试基线 + 安全配置 | ✅ 完成 | 源码可复现、离线测试通过 | 26 项测试通过；源码已脱敏 |
| 2 | ONNX 审计 + bmodel 转换 | ✅ 完成 | x86 F32 转换和数值验证通过 | `9255f27` 提交，bmodel 已跟踪 |
| 2B | 板端 bmodel 加载与兼容性验证 | ✅ 完成 | bmrt_test + bmrt_load_test 通过 | F32 bmodel SHA256 一致，板端真实加载正常 |
| 3 | 单图 C++ 推理 PoC | ✅ 完成 | 单图 PoC 通过 | 精度对照待 x86 基线 |
| 4 | 单路硬件视频管线 PoC | ⏳ 待启动 | 单路稳定 2h | 待阶段3 |
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
