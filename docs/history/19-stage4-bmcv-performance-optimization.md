# 阶段4 BMCV 性能优化报告

> 阶段4.2 RTSP 输入复用本管线 h264_bm 硬解 + CPU 预处理 + BMCV 绘框 + h264_bm 硬编链路（推荐配置不变），详见 `21-stage4-2-single-rtsp-input.md`。

> 状态：阶段4 本地单路视频功能通过；短时 300 秒验证通过；完整 30 分钟和 2 小时门禁待人工执行。

## 1. 背景

阶段4 单路视频管线在 CPU 路径下输出约 5fps，低于 10fps 目标。探测确认主要瓶颈为板端 `libswscale` 无 SIMD 优化，导致 NV12<->RGB 颜色空间转换极慢：

| 操作 | 耗时 |
|---|---|
| sws NV12→RGB 960×544（绘框输入） | 97ms |
| sws RGB→NV12 960×544（绘框写回） | 35ms |
| sws NV12→RGB 640×640（推理输入） | 73ms |
| **绘框往返合计** | **132ms/帧** |
| CPU 归一化（原三次遍历+pixel()） | 22ms |

每秒 CPU 预算：10×132（绘框）+ 5×(73+22+16)（推理）≈ 1875ms/s，故实际仅约 5fps。

## 2. BMCV 能力探测（libsophon 0.4.9）

基于 `/opt/sophon/libsophon-0.4.9/include` 板端真实头文件，逐项实测：

| BMCV API | 结果 | 耗时 |
|---|---|---|
| `bmcv_image_storage_convert`（NV12→RGB_PACKED CSC） | ✅ 可用 | 0.3ms |
| `bmcv_image_draw_rectangle`（NV12 上绘制矩形） | ✅ 可用 | 0.4ms |
| `bmcv_image_vpp_basic`（VPP CSC+resize） | ❌ 失败 | — |
| `bmcv_image_vpp_basic_v2`（float /255 一站式） | ❌ 不支持 | — |
| `bmcv_image_yuv_resize`（YUV resize） | ❌ 不支持 | — |
| `bmcv_image_resize`（RGB resize） | ❌ 不支持 | — |
| `bmcv_image_convert_to`（归一化） | ❌ 不支持 RGB_PACKED | — |

**结论**：BMCV 可做 CSC（颜色转换）和绘制，但 **无法做 resize**。VPP 在本板普遍不可用（即使合成 128×128→64×64 也失败，报"scaling ratio greater than 32"）。resize 必须由 libswscale 完成。

## 3. 双路径实现

### 3.1 CLI 选项

```
--preprocess cpu|bmcv   # 预处理路径，默认 cpu
--draw-mode cpu|bmcv|none  # 绘制路径，默认 cpu
```

### 3.2 BMCV 预处理（`--preprocess bmcv`）

链路：NV12(host) → H2D(DDR1) → BMCV CSC → RGB_PACKED 960×544(DDR0) → D2H → sws resize → 640×640 → CPU 归一化 /255 NCHW

| 步骤 | 耗时 |
|---|---|
| H2D NV12 960×544 | 5ms |
| BMCV CSC | 0.3ms |
| D2H RGB_PACKED 960×544 | 1ms |
| sws RGB 960×544 → 640×640 | 38ms |
| CPU 归一化（优化版） | 7ms |
| **合计** | **~51ms** |

vs CPU 路径 73+7 = 80ms，节省约 29ms/推理帧。

### 3.3 BMCV 绘制（`--draw-mode bmcv`）

链路：NV12(host) → H2D(DDR0) → BMCV draw_rectangle → D2H 写回 AVFrame

| 步骤 | 耗时 |
|---|---|
| H2D NV12 | 5ms |
| BMCV draw_rectangle | 0.4ms |
| D2H NV12 | 0.5ms |
| **合计** | **~6ms** |

vs CPU 路径 132ms/帧，**节省 126ms/帧**。这是性能提升的关键。

### 3.4 CPU 归一化优化

原实现三次遍历 + `pixel()` 边界检查约 22ms，优化为单次遍历直接指针访问约 7ms（3× 加速）。

### 3.5 资源复用

`BmcvProcessor`（PIMPL）在 `init` 时创建所有 bm_image 和 sws 上下文，每帧复用，析构统一释放。禁止每帧分配大块设备/主机内存。

## 4. 正确性对照

使用 `preprocess_compare` 工具对 41 帧（每 5 帧取一帧）分别运行 CPU 和 BMCV 预处理 → 推理 → 后处理，比较检测结果。

| 指标 | 结果 |
|---|---|
| 无检测帧（32 帧） | ✅ 全部一致（0=0） |
| 有检测帧（9 帧） | ❌ IoU 0.94–0.98（门禁 0.98），score 差 0.02–0.09（门禁 0.01） |
| 漏检 | 2 帧（CPU 检出 1，BMCV 检出 0） |
| NaN/Inf | 无 |

**根因分析**：BMCV CSC（BT601 YCbCr）与 sws 内部 CSC 系数存在差异（RGB 960×544 均值差 1.56/255 = 0.6%），经 CNN 放大后导致 score 和框坐标偏移。resize 顺序差异（一步 CSC+resize vs 两步 CSC→resize）贡献较小（均值差 0.53/255）。

**结论**：BMCV 预处理正确性对照未通过门禁，**CPU 保持为默认预处理路径**。BMCV 预处理仅用于性能对比。

BMCV 绘制（`--draw-mode bmcv`）不影响检测语义（仅在输出视频上画框），正确性无争议。

## 5. 推荐配置

```
--preprocess cpu --draw-mode bmcv
```

CPU 预处理保证检测正确性，BMCV 绘制消除 sws 往返瓶颈。

## 6. 性能测试结果

### 60 秒性能对比

| 指标 | CPU+BMCV（推荐） | BMCV+BMCV（性能） |
|---|---|---|
| 输出 fps | 10.02 | 10.03 |
| 推理 fps | 5.00 | 5.00 |
| 预处理均值 | 76ms | 49ms |
| 推理均值 | 15.9ms | 16.3ms |
| 端到端 P95 | 150ms | 122ms |
| CPU 占用 | ~45% | ~45% |
| RSS | ~26MB | ~26MB |
| 丢帧 | 2 | 1 |
| 退出码 | 0 | 0 |

### 300 秒稳定性（推荐配置 cpu+bmcv）

| 指标 | 结果 | 门禁 |
|---|---|---|
| 输出帧 | 3001 | — |
| 输出 fps | 10.003 | 9.5–10.5 ✅ |
| 推理 fps | 5.003 | ~5 ✅ |
| 推理 P95 | <25ms | <25ms ✅ |
| 端到端 P95 | 150ms | <500ms ✅ |
| 退出码 | 0 | 0 ✅ |
| 队列 | 0 | ≤1 ✅ |
| RSS 增长 | 356KB/240s | 无持续增长 ✅ |
| 线程数 | 6（恒定） | 无泄漏 ✅ |
| 残留进程 | 无 | 无 ✅ |
| ffprobe | 通过 | 通过 ✅ |

## 7. 新增文件

| 文件 | 说明 |
|---|---|
| `include/video/bmcv_processor.h` | BMCV 预处理/绘制封装（PIMPL） |
| `src/video/bmcv_processor.cpp` | 实现 |
| `tools/video_inference/preprocess_compare.cpp` | CPU/BMCV 正确性对照工具 |
| `tests/unit_cpp/test_bmcv_processor.cpp` | BMCV 单元测试 |
| `tests/test_stability_script.sh` | 稳定性脚本行为测试 |

## 8. 最终结论

- BMCV 绘制路径功能通过，性能达标（10fps），300 秒短时测试通过。
- BMCV 预处理路径功能通过但正确性对照未过门禁（CSC 系数差异），CPU 保持默认。
- 推荐配置 `--preprocess cpu --draw-mode bmcv` 达到 10fps 性能目标。
- 完整 30 分钟和 2 小时门禁待用户手动执行。
