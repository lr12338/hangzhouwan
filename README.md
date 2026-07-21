# 杭州湾船舶检测 BM1684 边缘迁移项目

> 仓库：<https://github.com/lr12338/hangzhouwan> ｜ 分支：`feat/bm1684-edge-deployment` ｜ 目标芯片：**BM1684（chipid `0x1684`，非 BM1684X）**
>
> 本仓库由原 Windows + ONNX Runtime(CUDA) 的 Python 船舶检测系统，向算能 **BM1684 SoC 工控机**迁移。新生产路径优先采用 **C++ BMRuntime 全栈热路径**。

**新开发者请先读本文件，再读 [docs/00-index.md](docs/00-index.md) 与 [docs/PROGRESS.md](docs/PROGRESS.md)。**

---

## 1. 项目一句话

将杭州湾大桥南北向双路摄像机视频的船舶检测 + 坐标映射 + AIS 关联系统，从 Windows/CUDA 迁移到 BM1684 工控机，目标为板端 C++ BMRuntime 推理（F32 基线 → INT8 生产），不依赖 SAIL，不使用 FP16。

## 2. 迁移阶段总览（截至 2026-07-21）

| # | 阶段 | 状态 | 门禁 |
|---|---|---|---|
| 1 | 测试基线 + 安全配置 | ✅ 完成 | 源码可复现、离线测试通过 |
| 2 | ONNX 审计 + bmodel 转换 | 🔶 板端就绪 / ⛔ 待 x86 | 板端 `bmrt_test` + 精度通过 |
| 3 | 单图 C++ 推理 PoC | ⏳ 待启动 | 单图 PoC 通过 |
| 4 | 单路硬件视频管线 PoC | ⏳ | 单路稳定 2h |
| 5 | 坐标映射 + AIS 迁移 | ⏳ | 关联不低于旧版 |
| 6 | 双路 Pipeline 整合 | ⏳ | 双路运行 |
| 7 | 生产部署 + 无人值守 | ⏳ | 部署就绪 |
| 8 | 分级稳定性验证 | ⏳ | 验收线达标 |
| 9 | 代码收口 + GitHub 交付 | ⏳ | PR 合并 |

图例：✅ 完成 · 🔶 进行中/部分完成 · ⛔ 阻塞 · ⏳ 待启动

> **当前阻塞**：阶段2 bmodel 转换须在 x86 执行 TPU-MLIR（工控机为 SoC，不装转换工具链）。待 x86 运行 `tools/convert_model/convert_bmodel.sh` 产出 bmodel → 拷回板端 → `bmrt_test` 验证。

## 3. 当前状态摘要

- **阶段1 通过**：源码已解除对明文凭据、Windows 路径、固定生产地址的依赖；新增中文日志、配置校验、脱敏器、测试框架；26 项离线测试全部通过。
- **阶段2 板端就绪**：`best.onnx` 已审计（动态输入须固化为 `[1,3,640,640]`，输出不含 NMS）；63 帧真实数据校准集就绪；x86 转换脚本就绪；板端最小 C++ BMRuntime 加载程序编译并通过 API 链路验证（无 bmodel，退出码 0）。
- **未 push**：`feat/bm1684-edge-deployment` 共 11 个本地提交，尚未推送到远程。

## 4. 快速开始

```bash
cd /home/linaro/hangzhouwan-orign/hangzhouwan
git switch feat/bm1684-edge-deployment

# 运行离线测试（26 项，纯标准库，无需 pip install）
python3 tests/run_tests.py

# ONNX 审计（纯标准库 protobuf 解析，无需 onnx 包）
python3 tools/inspect_onnx.py weights/best.onnx

# 校验示例配置
python3 tools/validate_config.py config/application.example.yaml

# 敏感信息扫描
python3 tools/redact_secrets.py --scan hangzhouwan_beishang/
```

### 板端 C++ 加载程序（须有 bmodel）

```bash
# 编译
g++ -std=c++14 -I/opt/sophon/libsophon-0.4.9/include \
  tools/image_inference/bmrt_load_test.cpp -o tools/image_inference/bmrt_load_test \
  -L/opt/sophon/libsophon-0.4.9/lib -lbmrt -lbmlib -lpthread -ldl

# 运行（bmodel 到位后）
LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib tools/image_inference/bmrt_load_test weights/*_1684_f32.bmodel

# Sophon 自带 bmrt_test
/opt/sophon/libsophon-current/bin/bmrt_test --bmodel weights/*_1684_f32.bmodel --devid 0
```

### x86 bmodel 转换（须在 x86 执行，不在工控机）

```bash
# x86 开发机上，TPU-MLIR 与 libsophon 0.4.9 匹配版本
bash tools/convert_model/convert_bmodel.sh
# 产出 weights/yolov7_ship_1684_f32.bmodel 与 weights/yolov7_ship_1684_int8.bmodel
```

## 5. 目录结构

```
hangzhouwan/
├── README.md                          # 本文件（项目入口）
├── config/                            # 配置模板（阶段1新增）
│   ├── application.example.yaml       # 应用配置示例（streams 默认 disabled）
│   ├── logging.example.yaml           # 日志配置示例
│   └── README.md
├── docs/                              # 迁移文档（见 docs/00-index.md）
│   ├── 00-index.md                    # 文档导航
│   ├── 01-migration-gap-analysis.md   # 迁移差距分析（带文件:行号）
│   ├── 03-project-assets-inventory.md # 项目资产清单
│   ├── 04-stage1-full-project-baseline.md   # 阶段1 基线报告
│   ├── 05-model-and-test-assets-status.md   # 模型与测试资产状态
│   ├── 06-stage2-onnx-audit-and-bmodel.md   # 阶段2 ONNX审计与bmodel
│   ├── PROGRESS.md                    # 阶段进度跟踪（唯一权威索引）
│   ├── AGENT_HANDOFF.md               # Agent 接手提示词
│   └── credential-rotation-checklist.md     # 凭据轮换清单
├── requirements/                      # 依赖分类（阶段1新增）
│   ├── baseline-x86.txt               # x86 旧模型基线环境
│   ├── tools.txt                      # 工具环境
│   └── README.md
├── tools/                             # 工具脚本（阶段1/2新增）
│   ├── inspect_onnx.py                # ONNX 审计（纯标准库）
│   ├── redact_secrets.py              # 敏感信息脱敏/扫描
│   ├── validate_config.py             # 配置校验
│   ├── convert_model/                 # bmodel 转换（x86 执行）
│   │   ├── extract_calibration.sh     # 从 test.mp4 抽帧做 INT8 校准集
│   │   └── convert_bmodel.sh          # x86 TPU-MLIR F32->INT8 转换
│   └── image_inference/               # 板端 C++ 推理
│       ├── bmrt_load_test.cpp         # 最小 BMRuntime 加载程序
│       └── README.md
├── tests/                             # 测试框架（阶段1新增）
│   ├── run_tests.py                   # 测试入口（26 项）
│   ├── unit/                          # 单元测试（脱敏、配置校验）
│   ├── integration/                   # 集成测试（资产、ONNX 审计）
│   ├── expected/                      # 结果 schema
│   └── fixtures/                      # 测试夹具
├── testdata/                          # 测试数据
│   ├── test.mp4                       # H264 960×544@20fps，125s
│   └── calibration/                   # INT8 校准帧（63 帧，gitignore）
├── weights/                           # 模型与字体（gitignore 大文件）
│   ├── best.onnx                      # 检测模型（用户补齐，gitignore）
│   ├── 0121_random_forest_model.pkl   # A 路坐标模型（gitignore）
│   ├── beishang_x-l.pkl               # B 路坐标模型（gitignore）
│   ├── beet0110.pt                    # PyTorch 权重（gitignore）
│   └── simhei.ttf                     # 中文字体（已跟踪）
├── hangzhouwan_beishang/              # 【legacy 只读参考】原业务源码，不在其上打补丁
│   ├── starter_optimized.py           # 原生产入口（禁止运行，会连正式流）
│   ├── stream_handler_optimized.py    # 原生产流处理（3 线程 + libx264）
│   ├── detector.py                    # 原 ONNX Runtime(CUDA) 推理封装
│   ├── getais.py                      # 原 AIS/MQTT
│   ├── find_ship.py                   # 原坐标随机森林
│   └── plot.py                        # 原绘框
├── yolov7_requirements.txt            # legacy Windows conda 依赖
└── nginx 1.7.11.3 Gryphon/            # legacy Windows nginx-rtmp 源码树（与迁移无关，待剔除）
```

## 6. 关键约束（务必遵守）

- 目标芯片 **BM1684**，处理器参数 `--processor BM1684`，**不得 BM1684X**。
- 模型路线 **F32 基线 → INT8 生产**，**不得 FP16**（FP16 为 BM1684X 专有）。
- **不在工控机安装** TPU-MLIR / torch / onnxruntime-gpu / CUDA / Python SAIL。
- **不在工控机转换 bmodel**（TPU-MLIR 须在 x86 执行）。
- 不连接正式 RTSP / MQTT / RTMP；不修改 systemd；不重写 Git 历史；不自动 push；不提交真实凭据。
- 不运行 `starter_optimized.py`、`start_optimized.bat`（会连正式流）。
- 所有新增日志使用中文；敏感信息须脱敏；示例配置中所有 stream 默认 `disabled`。

## 7. 环境基线（工控机，阶段0 确认）

| 项 | 值 |
|---|---|
| 系统 | Ubuntu 20.04 aarch64 厂商定制 |
| 芯片 | BM1684（chipid `0x1684`，C++ `bm_get_chipid` 三次稳定返回） |
| libsophon | 0.4.9 LTS |
| BMRuntime | `libbmrt.so.1.0` |
| Sophon-FFmpeg / OpenCV | 0.8.0 |
| 编解码 | `h264_bm` / `h265_bm` 硬件编解码、BMCV/BMVideo/BMVPU/VPP 完整 |
| Python | 3.8.2（仅 numpy / PyYAML / psutil，无 cv2/onnx/torch/joblib/sklearn） |
| C++ 编译 | 完整（gcc，`-lbmrt -lbmlib`） |
| 可用内存 | 约 2.2 GiB；`/data` 约 15 GB |
| TPU-MLIR | **未安装**（x86-only，工控机不装） |

## 8. 测试进展

- 框架：Python 标准库 `unittest`（无需 pytest）。
- 入口：`python3 tests/run_tests.py`。
- 当前：**26 项全部通过**（10 脱敏 + 10 配置校验 + 4 资产 + 2 ONNX 审计回归）。
- 覆盖：敏感信息脱敏、配置字段校验与生产门禁、字体/视频资产存在性与可解码性、ONNX 审计关键结论回归。

## 9. 后续任务

1. **【阻塞解除】x86 执行 bmodel 转换**：在 x86 开发机运行 `tools/convert_model/convert_bmodel.sh`，产出 F32/INT8 bmodel。
2. **板端验证**：bmodel 拷回 `weights/`，运行 `bmrt_test` 与 `bmrt_load_test` 验证加载与精度（闭合阶段2 门禁）。
3. **阶段3 单图 C++ PoC**：BMRuntime 推理 + 后处理（xywh→xyxy、NMS、rescale）+ 绘框，因模型不含 NMS 须 C++ 侧实现。
4. **阶段4–9**：按 `docs/PROGRESS.md` 推进。
5. **凭据轮换**：仓库为 public，源码历史中含真实凭据（已从源码清除但未重写历史），须按 `docs/credential-rotation-checklist.md` 轮换。

## 10. 文档导航

详见 [docs/00-index.md](docs/00-index.md)。核心文档：
- [docs/PROGRESS.md](docs/PROGRESS.md) — 阶段进度唯一权威索引
- [docs/01-migration-gap-analysis.md](docs/01-migration-gap-analysis.md) — 迁移差距与问题位置
- [docs/04-stage1-full-project-baseline.md](docs/04-stage1-full-project-baseline.md) — 阶段1 基线报告
- [docs/06-stage2-onnx-audit-and-bmodel.md](docs/06-stage2-onnx-audit-and-bmodel.md) — 阶段2 审计与转换
