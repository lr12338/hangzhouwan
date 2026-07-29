# 杭州湾船舶检测 BM1684 边缘迁移项目

> 仓库：<https://github.com/lr12338/hangzhouwan> ｜ 分支：`feat/bm1684-edge-deployment` ｜ 目标芯片：**BM1684（chipid `0x1684`，非 BM1684X）**
>
> 本仓库由原 Windows + ONNX Runtime(CUDA) 的 Python 船舶检测系统，向算能 **BM1684 SoC 工控机**迁移。新生产路径优先采用 **C++ BMRuntime 全栈热路径**。

**新开发者请先读本文件，再读 [docs/00-index.md](docs/00-index.md) 与 [docs/PROGRESS.md](docs/PROGRESS.md)。**

---

## 1. 项目一句话

将杭州湾大桥南北向双路摄像机视频的船舶检测 + 坐标映射 + AIS 关联系统，从 Windows/CUDA 迁移到 BM1684 工控机，目标为板端 C++ BMRuntime 推理（F32 基线 → INT8 生产），不依赖 SAIL，不使用 FP16。

## 2. 迁移阶段总览（截至 2026-07-23）

| # | 阶段 | 状态 | 门禁 |
|---|---|---|---|
| 1 | 测试基线 + 安全配置 | ✅ 完成 | 源码可复现、离线测试通过 |
| 2 | ONNX 审计 + bmodel 转换 | ✅ 完成 | x86 F32 转换和数值验证通过 |
| 2B | 板端 bmodel 加载与兼容性验证 | ✅ 完成 | bmrt_test + bmrt_load_test 通过 |
| 3 | 单图 C++ 推理 PoC | ✅ 完成 | 单图 PoC 通过（精度对照待 x86 基线） |
| 4 | 单路硬件视频管线 PoC | ✅ | 功能通过，已集成双路生产 |
| 4.2 | 单路 RTSP 输入到本地文件 | ✅ | RTSP 取流+重连已上线生产 |
| 5 | 坐标映射 + AIS 迁移 | ✅ 完成 | sklearn 坐标预测 + AIS/MQTT 订阅已上线运行 |
| 6 | 双路 Pipeline 整合 | ✅ 完成 | A/B 双路正式推流运行中 |
| 7 | 生产部署 + 无人值守 | ✅ | 生产运行中，开机自启已 enable |
| 8 | 分级稳定性验证 | 🔶 | 短时验证通过，长稳 2-4h 待维护窗口 |
| 9 | 代码收口 + GitHub 交付 | ✅ | 已推送 `feat/bm1684-edge-deployment` |

图例：✅ 完成 · 🔶 进行中/部分完成 · ⏳ 待启动

> **阶段4**：单路硬件视频管线功能通过，BMCV 绘制优化达 10fps，短时 300s 验证通过。完整 30min/2h 长时门禁待人工执行。详见 `docs/history/19`、`docs/history/20`。
>
> **阶段4.2**：单路 RTSP 输入到本地文件——RTSP 源（连接/读取超时、受控断线重连、URL 凭据脱敏、环境变量安全输入、输出 PTS 单调）代码与单元测试完成，RTSP 连接/超时/中断/脱敏路径已用受控无效地址验证；实流解码与中途断线重连、30min/2h 门禁待人工执行（见 `docs/history/21`、`docs/history/22`）。不接 RTMP/MQTT/AIS/双路。

## 3. 当前状态摘要

- **阶段1 通过**：源码已解除对明文凭据、Windows 路径、固定生产地址的依赖；新增中文日志、配置校验、脱敏器、测试框架；离线测试全部通过（运行 `python3 tests/run_tests.py` 或 `python3 -m pytest tests/`）。
- **阶段2 完成**：ONNX 审计通过；F32 bmodel 已生成（`artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel`，24,428,544 字节，SHA256 `d1c295c5...`）；ONNX/MLIR 172 个 Tensor 数值验证通过；bmodel 已提交至 Git（`9255f27`）。
- **阶段2B 完成**：`bmrt_test` 和 `bmrt_load_test` 在板端真实加载成功，网络 `yolov7_ship`，输入 `images [1,3,640,640] FLOAT32`，输出 `output_Concat [1,25200,6] FLOAT32`，无兼容性错误，TPU 资源正常释放。
- **阶段3 完成**：C++ 单图推理 PoC 完成（BmrtDetector + YOLOv7 后处理 + 绘框），预处理 56ms，推理 16ms，后处理 0.6ms；100 次重复推理平均 15.99ms，框数稳定，无内存泄漏；C++ 单元测试和 Python 测试全部通过（`cd build && ctest` + `python3 -m pytest tests/`）。
- **阶段4 进行中**：单路硬件视频管线功能通过；BMCV 绘制优化（`--draw-mode bmcv`）消除 sws 往返瓶颈，输出达 10fps；短时 300s 验证通过（10.003fps，退出码 0，RSS +356KB，P95=150ms 稳定，无残留）；完整 30min/2h 长时门禁待人工执行。
- **阶段4.2 进行中**：单路 RTSP 输入（`--source-type rtsp --input-env HZW_TEST_RTSP_URL`）已实现连接/读取超时（`stimeout`）、受控断线重连（指数退避）、URL 凭据脱敏、停止信号中断阻塞、输出 PTS 单调；`ctest` + Python 测试全通过，20s 文件回归通过，受控无效地址连接路径验证通过；实流与长时门禁待人工执行。

## 3.5 生产运维文档

| 文档 | 用途 |
|---|---|
| [生产运维手册](docs/production/operations-guide.md) | 启动、停止、状态、日志、配置、升级、回滚、故障处理 |
| [维护窗口 Runbook](docs/production/maintenance-window-runbook.md) | VPU 重连修复门禁/步骤/回滚 |
| [部署审计证据](docs/production/maintenance-window-audit-evidence.md) | 生产部署记录（1eba419/9d449ab/c008eac） |
| [人工长时测试指南](docs/production/manual-long-run-guide.md) | L1-L4 长测执行步骤和验收标准 |

**快速操作：**
- 启动服务：`sudo systemctl start hangzhouwan.target`
- 查看状态：`/opt/hangzhouwan/current/bin/hzwctl status`
- 查看日志：`journalctl -u hangzhouwan-video.service -f`
- 升级 Release：`sudo bash tools/release/activate_release.sh <candidate>`
- 回滚 Release：`sudo bash tools/release/rollback_release.sh`

---

## 4. 快速开始

```bash
cd /home/linaro/hangzhouwan
git switch feat/bm1684-edge-deployment

# 运行离线测试（纯标准库，无需 pip install）
python3 tests/run_tests.py

# C++ 构建与测试
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
cd build && ctest --output-on-failure

# 单图推理
LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib \
  ./build/single_image_infer \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --image testdata/model_test/frame_30.jpg \
  --output result.jpg --json result.json \
  --conf 0.1 --iou 0.1 --device 0

# ONNX 审计（纯标准库 protobuf 解析，无需 onnx 包）
python3 tools/inspect_onnx.py weights/best.onnx

# 校验示例配置
python3 tools/validate_config.py config/application.example.yaml

# 敏感信息扫描
python3 tools/redact_secrets.py --scan .
```

### 板端 C++ 加载程序（须有 bmodel）

```bash
# 手动编译（CMake 构建见上方快启）：
g++ -std=c++14 -I/opt/sophon/libsophon-0.4.9/include \
  tools/image_inference/bmrt_load_test.cpp -o tools/image_inference/bmrt_load_test \
  -L/opt/sophon/libsophon-0.4.9/lib -lbmrt -lbmlib -lpthread -ldl

# 运行（bmodel 到位后）
LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib tools/image_inference/bmrt_load_test artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel
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
├── CMakeLists.txt                     # C++ 构建配置（阶段3新增）
├── config/                            # 配置模板（阶段1新增）
│   ├── application.example.yaml       # 应用配置示例（streams 默认 disabled）
│   ├── logging.example.yaml           # 日志配置示例
│   └── README.md
├── include/                           # C++ 头文件（阶段3新增）
│   ├── image_io/jpeg_io.h             # JPEG 读写与绘图
│   ├── inference/bmrt_detector.h      # BmrtDetector 封装
│   ├── inference/yolov7_postprocess.h # YOLOv7 后处理
│   └── util/sha256.h                  # SHA-256
├── src/                               # C++ 源码（阶段3新增）
│   ├── image_io/jpeg_io.cpp
│   ├── inference/bmrt_detector.cpp
│   ├── inference/yolov7_postprocess.cpp
│   └── util/sha256.cpp
├── docs/                              # 迁移文档（见 docs/00-index.md）
│   ├── 00-index.md                    # 文档导航
│   ├── 01-migration-gap-analysis.md   # 迁移差距分析（带文件:行号）
│   ├── 03-project-assets-inventory.md # 项目资产清单
│   ├── 04-stage1-full-project-baseline.md   # 阶段1 基线报告
│   ├── 05-model-and-test-assets-status.md   # 模型与测试资产状态
│   ├── 06-stage2-onnx-audit-and-bmodel.md   # 阶段2 ONNX审计与bmodel
│   ├── 15-stage2b-board-model-validation.md  # 阶段2B 板端模型验证
│   ├── 16-stage3-single-image-cpp-poc.md     # 阶段3 单图C++推理PoC
│   ├── PROGRESS.md                    # 当前生产状态权威索引
│   ├── AGENT_HANDOFF.md               # Agent 接手提示词
│   └── credential-rotation-checklist.md     # 凭据轮换清单
├── requirements/                      # 依赖分类（阶段1新增）
├── tools/                             # 工具脚本
│   ├── image_inference/
│   │   ├── README.md                  # 板端加载程序说明
│   │   ├── bmrt_load_test.cpp         # 最小 BMRuntime 加载验证
│   │   └── single_image_infer.cpp     # 单图推理 PoC（阶段3新增）
│   ├── convert_model/                 # x86 TPU-MLIR 转换脚本
├── tests/                             # 测试框架
│   ├── unit/                          # Python 单元测试
│   ├── integration/                   # Python 集成测试
│   ├── unit_cpp/                      # C++ 单元测试（阶段3新增）
│   └── run_tests.py                   # 测试入口
├── testdata/                          # 测试数据
│   └── test.mp4                       # 测试视频（960×544, 125s）
├── artifacts/                         # 模型资产（bmodel 入库，其余 gitignore）
│   └── bm1684-f32/yolov7_ship_1684_f32.bmodel
├── deploy/                            # 部署脚本与 systemd unit
├── services/                          # Business 富化 Python 服务
└── weights/                           # 模型文件（gitignore，不入库）
    ├── best.onnx                      # 原始 ONNX（本地存在）
    ├── 0121_random_forest_model.pkl   # A 路坐标模型
    └── beishang_x-l.pkl               # B 路坐标模型
```

## 6. 执行红线

**永久禁止：**
- 安装 TPU-MLIR、Python SAIL、Torch、CUDA 或 onnxruntime-gpu
- 修改板端 libsophon
- 为适配模型升级或替换厂商运行时
- 硬编码输出 Tensor 名称为 `output`
- 在板端测试失败时伪造成功结果
- 未验证精度时声称模型精度完全一致
- 提交 `/etc/hangzhouwan/` 真实配置和凭据到 Git

**开发阶段限制（生产已解除）：**
- ~~连接正式 RTSP、MQTT、RTMP~~ — 生产已切换正式推流地址
- ~~修改 systemd~~ — systemd unit 已安装（未 enable，待 L4 + Windows 回切演练通过后 enable）
- ~~执行 INT8 转换~~ — 当前使用 F32 bmodel，INT8 转换为后续优化项

**所有运行日志和报告使用中文。**
