# 迁移进度跟踪（PROGRESS）

> 本文件是**当前测试进展的唯一权威索引**。每完成一个阶段或关键决策，更新本文件。新开发者先读 [../README.md](../README.md)，再读本文件，再按 [00-index.md](00-index.md) 深入。

| 项 | 值 |
|---|---|
| 最后更新 | 2026-07-21（离线镜像导入与 F32 转换门禁复核） |
| 仓库 | `https://github.com/lr12338/hangzhouwan.git` |
| 本地路径 | `/home/huangchao/hangzhouwan/hangzhouwan` |
| 当前分支 | `feat/bm1684-edge-deployment` |
| HEAD | `b7a9fbc`（本次文档更新前） |
| 当前阶段 | **阶段2 F32 转换⛔官方 TPU-MLIR wheel 下载失败** |
| 总体健康度 | 🟡 阶段1 闭合，阶段2 未转换 |

---

## 1. 阶段总览

| # | 阶段 | 状态 | 门禁 | 备注 |
|---|---|---|---|---|
| 0 | 项目与设备基线确认 | ✅ 完成 | 通过 | 三份文档 + chip 探针已交付（见外部 `hangzhouwan_src/docs`） |
| 1 | 测试基线 + 安全配置 | ✅ 完成 | 源码可复现、离线测试通过 | 26 项测试通过；源码已脱敏 |
| 2 | ONNX 审计 + bmodel 转换 | 🔶 板端就绪 / ⛔ 待 x86 | 板端 `bmrt_test` + 精度通过 | 审计/校准集/脚本/加载程序就绪；bmodel 须 x86 转换 |
| 3 | 单图 C++ 推理 PoC | ⏳ 待启动 | 单图 PoC 通过 | 待阶段2 |
| 4 | 单路硬件视频管线 PoC | ⏳ | 单路稳定 2h | 待阶段3 |
| 5 | 坐标映射 + AIS 迁移 | ⏳ | 关联不低于旧版 | 待阶段4 |
| 6 | 双路 Pipeline 整合 | ⏳ | 双路运行 | 待阶段5 |
| 7 | 生产部署 + 无人值守 | ⏳ | 部署就绪 | 待阶段6 |
| 8 | 分级稳定性验证 | ⏳ | 验收线达标 | 待阶段7 |
| 9 | 代码收口 + GitHub 交付 | ⏳ | PR 合并 | 待阶段8 |

图例：✅ 完成 · 🔶 进行中/部分完成 · ⛔ 阻塞 · ⏳ 待启动/待前置

---

## 2. 阶段1 完成项清单（✅ 通过）

- [x] 建立分支 `feat/bm1684-edge-deployment`；以本地完整 Git 仓库为唯一源码依据重新核验
- [x] 完整资产盘点（`docs/03`）：三模型缺失（后于阶段2 补齐）、字体/测试视频可用
- [x] 安全整改（不改变算法结果）：移除源码明文凭据（RTSP/MQTT/RTMP/萤石/Agora/API Token）、Windows 路径、固定生产地址（`alpha.hifleet.com`）
- [x] `except: pass` -> 受控中文日志；模型/字体路径集中管理；`streams` 默认 `disabled`
- [x] 新增 `config/`（application/logging 示例 + README）、`.env.example`、`requirements/`（baseline-x86 / tools / README）
- [x] 重写 `.gitignore`（UTF-8，新增 `.env`/`application.yaml`/`*.bmodel`/`logs`/`build`/`venv`）
- [x] 工具：`tools/redact_secrets.py`、`tools/validate_config.py`
- [x] 测试框架：`tests/run_tests.py`，**26 项全部通过**（纯标准库 `unittest`）
- [x] 文档：`docs/01`（差距分析，带文件:行号）、`docs/03`（资产）、`docs/04`（阶段1 报告，16 节）、`docs/05`（模型/测试状态）、`docs/credential-rotation-checklist.md`
- [x] 敏感信息扫描：源码 0 处明文命中（`git grep` 验证）

**阶段1 门禁：✅ 通过**（基础闭合，可进入阶段2 准备）。

> 仓库为 public，历史中含真实凭据（源码已清除但未重写历史）-> 须按 `docs/credential-rotation-checklist.md` 轮换，不重写历史。

---

## 3. 阶段2 完成项清单（🔶 受控转换⛔阻塞）

- [x] 模型补齐至 `weights/`（用户提供，gitignore）：`best.onnx`(24M)、`0121_random_forest_model.pkl`(7.6M)、`beishang_x-l.pkl`(2.1M)、`beet0110.pt`(74.7M)
- [x] ONNX 审计（`tools/inspect_onnx.py`，纯标准库 protobuf 解析）：ir7 / opset12 / 239 节点；输入 `images` FLOAT **动态 `[batch,3,height,width]`** -> 须固化 `[1,3,640,640]`；输出 `output` **不含 NMS**（后处理须 C++ 侧实现，阶段3）；算子均为 TPU-MLIR 常见支持
- [x] INT8 真实数据校准集：`tools/convert_model/extract_calibration.sh` 用 Sophon-FFmpeg 从 `testdata/test.mp4` 抽 **63 帧**（960×544，gitignore，可由脚本再生）
- [x] x86 转换脚本：`tools/convert_model/convert_bmodel.sh`（model_transform -> F32 bmodel -> run_calibration -> INT8 bmodel，`--processor BM1684`，无 FP16）
- [x] 板端最小 C++ 加载程序：`tools/image_inference/bmrt_load_test.cpp` 编译通过，板端运行打开 BM 设备 0、加载 `libcpuop.so`、API 链路正常、退出码 0（无 bmodel）
- [x] 芯片复核：`chipid=0x1684 model=BM1684`（与阶段0 一致）
- [x] 文档：`docs/06-stage2-onnx-audit-and-bmodel.md`；更新 `docs/05`
- [x] 测试：新增 ONNX 审计回归测试（2 项），总计 26 项通过

**2026-07-21 离线复核：**
- [x] 已校验并导入 `/home/huangchao/tpu-mlir-offline/tpuc_dev_v3.4.tar.gz`；本地完整性 SHA256 为 `d83e2cb55bc279076e516597363429c9bab76aae7999260046a786220b4819ac`
- [x] 实际镜像为 `sophgo/tpuc_dev:v3.4`，已标记为 `local/tpuc_dev:v3.4`；ID `sha256:d73afc9614a7e78e69dca61a9b7ac84ee9598c942571ea557c603850c5df59a1`，`amd64/linux`
- [ ] F32 转换：`docker load` 后立即空间曾为 9.4 GB，最终复核为 16 GB；每次重试前须确认至少 10 GB。镜像内未找到 TPU-MLIR 工具或环境脚本，禁止转换
- [x] 已将 `hangzhouwan_beishang/weights/best.onnx` 校验并复制至受控路径 `weights/best.onnx`，SHA256 为 `101f8e19c680eb4fd2446521f983b11ada35052bce56f30c2ddcf5df64894f92`
- [x] 已从 `testdata/test.mp4` 生成 `testdata/model_test/frame_000010.jpg`；该文件被 Git 忽略
- [ ] 官方 `tpu_mlir-1.28.1` wheel 下载失败：164,416,233 / 268,069,531 字节时出现 HTTP/2 `PROTOCOL_ERROR`，需要离线 wheel
- [ ] 受控 ONNX：`weights/best.onnx` 缺失。不同路径的 `hangzhouwan_beishang/weights/best.onnx` 不替代受控路径
- [ ] 数值验证：无测试图片，且主机和容器均无 `ffmpeg`，未生成参考输出或 bmodel 输出
- [ ] F32 交付包：未生成；未复制至板端

**待执行（板端，bmodel 到位后）：**
- [ ] `bmrt_test --bmodel ..._1684_f32.bmodel` 与 `bmrt_load_test ..._1684_f32.bmodel` 验证加载（满足"最小 C++ 加载成功"门禁）
- [ ] F32 精度与原 ONNX 对比通过 -> INT8 精度验证

**阶段2 门禁：⛔ 未生成 bmodel；禁止将任何 x86 或板端验证标记为通过。**

---

## 4. 当前阻塞与待办

### 阻塞（须外部输入才能解除）
| ID | 阻塞项 | 影响阶段 | 解除条件 | 负责方 |
|---|---|---|---|---|
| B3 | 官方 `tpu_mlir` wheel 下载失败 | 阶段2 | 提供与 Python 3.10 / Ubuntu 22.04 兼容的官方离线 wheel | 用户/运维 |
| B4 | 容器导入期间空间读数曾为 9.4 GB（最终复核 16 GB） | 阶段2 | 每次转换前复核至少 10 GB 可用空间 | 用户/运维 |
| B5 | 仓库历史含真实凭据（public） | 运维 | 按 `credential-rotation-checklist.md` 轮换凭据 | 用户/运维 |
| B6 | `weights/best.onnx` 缺失 | 阶段2 | 用户手动复制 ONNX 至受控路径 | 用户 |
| B7 | 无 JPG/JPEG/PNG 测试图片，无法从视频抽帧 | 阶段2 | 提供一张稳定图片或受控 ffmpeg 工具 | 用户/运维 |

> 阶段1 的 B1/B2（三模型缺失）已于阶段2 解除（用户补齐模型）。

### 待办（阶段2 解阻塞后）
- [ ] 条件全部满足后，仅执行 BM1684 F32 转换和 x86 原始 Tensor 数值验证
- [ ] 生成 F32 交付包；本次不执行板端验证或 INT8

---

## 5. 关键决策记录（ADR 摘要）

| 日期 | 决策 | 依据 |
|---|---|---|
| 2026-07-20 | 芯片定为 **BM1684（0x1684）**，非 BM1684X | C++ `bm_get_chipid` 权威返回 |
| 2026-07-20 | 模型路径 **F32 基线 -> INT8**，**禁用 FP16** | BM1684 无 FP16 支持（FP16 为 BM1684X 专有） |
| 2026-07-20 | 主方案 = **C++ 热路径全栈** | libbmrt+头+编解码+gcc 就绪；不依赖 SAIL；内存最小 |
| 2026-07-20 | 不在 SoC 装 SAIL / 不装模型转换工具链 | 任务要求；SAIL wheel 须严格匹配 0.4.9/cp38；转换在 x86 |
| 2026-07-20 | 原 Python 代码仅作 `legacy/` 参考，不在其上打补丁 | 任务要求新生产架构重新分层 |
| 2026-07-20 | 新仓库剔除 `nginx 1.7.11.3 Gryphon/`、`.bat`、重复实现 | Windows 专用或重复/空实现 |
| 2026-07-21 | ONNX 审计用自研标准库 protobuf 解析器 | 工控机无 `onnx` 且 PyPI 超时；不装禁止依赖 |
| 2026-07-21 | 动态输入固化 `[1,3,640,640]`，NMS 后处理移至 C++ | `best.onnx` 输入动态、输出不含 NMS |

---

## 6. 关键文件索引

| 文件 | 用途 |
|---|---|
| `README.md` | 项目入口 |
| `docs/00-index.md` | 文档导航 |
| `docs/01-migration-gap-analysis.md` | 迁移差距与问题位置（文件:行号） |
| `docs/03-project-assets-inventory.md` | 资产清单 |
| `docs/04-stage1-full-project-baseline.md` | 阶段1 基线报告 |
| `docs/05-model-and-test-assets-status.md` | 模型与测试资产状态 |
| `docs/06-stage2-onnx-audit-and-bmodel.md` | 阶段2 审计与转换 |
| `docs/credential-rotation-checklist.md` | 凭据轮换清单 |
| `docs/AGENT_HANDOFF.md` | Agent 接手提示词 |
| `tools/inspect_onnx.py` | ONNX 审计 |
| `tools/convert_model/convert_bmodel.sh` | x86 bmodel 转换 |
| `tools/image_inference/bmrt_load_test.cpp` | 板端 C++ 加载验证 |
| `tests/run_tests.py` | 测试入口（26 项） |

---

## 7. 约束速查（务必遵守）

- 目标 **BM1684**（`--processor BM1684`），不得 BM1684X；**F32 -> INT8**，不得 FP16。
- 不在工控机装 TPU-MLIR / torch / onnxruntime-gpu / CUDA / SAIL；不在工控机转换 bmodel。
- 不连正式 RTSP/MQTT/RTMP；不改 systemd；不重写 Git 历史；不自动 push；不提交真实凭据。
- 不运行 `starter_optimized.py` / `start_optimized.bat`。
- 新增日志用中文；敏感信息脱敏；streams 默认 disabled。
