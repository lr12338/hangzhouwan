# Agent 接手提示词（AGENT_HANDOFF）

> 复制下方「接手指令」整段给新 Agent，即可让其快速理解并接手本项目。

---

## 接手指令

你正在接手「杭州湾船舶检测 BM1684 边缘迁移」项目。请严格按以下信息行动，先理解现状再动手，禁止臆测。

### 1. 仓库与分支
- 本地仓库根：`/home/linaro/hangzhouwan-orign/hangzhouwan`
- 远程：`https://github.com/lr12338/hangzhouwan.git`（public，未 push）
- 分支：`feat/bm1684-edge-deployment`（由 `main` `56d380f` 创建，11 本地提交，HEAD `bf031af`，工作区应干净）
- 接手第一步：`cd` 到仓库根，运行 `git status`、`git log --oneline -12` 核对状态。

### 2. 必读文档（按顺序）
1. `README.md` - 项目入口、快速开始、目录、约束
2. `docs/PROGRESS.md` - **阶段进度唯一权威索引**、阻塞、ADR
3. `docs/06-stage2-onnx-audit-and-bmodel.md` - 阶段2 现状
4. `docs/01-migration-gap-analysis.md` - 迁移差距与问题位置（带文件:行号）

### 3. 平台与芯片（核心约束）
- 芯片为 **BM1684**（chipid `0x1684`），**不是 BM1684X**。所有 `--processor` 必须为 `BM1684`。
- 模型路线 **F32 基线 -> INT8**，**禁止 FP16**（FP16 是 BM1684X 专有）。
- 工控机 = SoC 本身：libsophon 0.4.9、`libbmrt.so.1.0`、Sophon-FFmpeg/OpenCV 0.8.0、C++ 编译完整；Python 3.8 仅有 numpy/PyYAML/psutil（无 cv2/onnx/torch/joblib/sklearn）。
- **禁止**在工控机安装 TPU-MLIR / torch / onnxruntime-gpu / CUDA / Python SAIL；**禁止**在工控机转换 bmodel（TPU-MLIR 为 x86-only）。
- **禁止**连接正式 RTSP/MQTT/RTMP；禁止改 systemd；禁止重写 Git 历史；禁止 `git push`；禁止提交真实凭据；禁止运行 `starter_optimized.py` 或 `start_optimized.bat`（会连正式流）。
- 新增日志用中文；敏感信息脱敏；示例配置中所有 stream 默认 `disabled`。

### 4. 当前状态（截至 2026-07-21）
- **阶段1（测试基线+安全配置）：✅ 完成**。源码已脱敏、配置校验/脱敏器/测试框架就绪，26 项离线测试全部通过（`python3 tests/run_tests.py`）。
- **阶段2（ONNX 审计+bmodel）：🔶 板端就绪 / ⛔ 阻塞于 x86**。
  - 已完成：`best.onnx` 审计（输入动态须固化 `[1,3,640,640]`，输出不含 NMS）、63 帧校准集、x86 转换脚本、板端最小 C++ 加载程序（编译并验证 API 链路，退出码 0）。
  - **阻塞**：bmodel 转换须在 x86 执行 `tools/convert_model/convert_bmodel.sh`，产出的 bmodel 拷回板端 `weights/` 后才能 `bmrt_test` 验证加载与精度。
- 模型已就位（gitignore，不入库）：`weights/best.onnx`、`weights/0121_random_forest_model.pkl`、`weights/beishang_x-l.pkl`、`weights/beet0110.pt`。
- 测试视频：`testdata/test.mp4`（H264 960×544@20fps，125s）；校准帧：`testdata/calibration/`（63 帧，gitignore）。

### 5. 接手后应做什么（取决于你被指派的任务）
- **若任务是「解除阶段2阻塞」**：需 x86 开发机。在 x86 安装与 libsophon 0.4.9 兼容的 TPU-MLIR，运行 `tools/convert_model/convert_bmodel.sh`，产出 `weights/yolov7_ship_1684_f32.bmodel` 与 `..._int8.bmodel`，拷回板端。然后在板端运行 `/opt/sophon/libsophon-current/bin/bmrt_test --bmodel weights/*_1684_f32.bmodel --devid 0` 与 `LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib tools/image_inference/bmrt_load_test weights/*_1684_f32.bmodel`，验证加载与精度。
- **若任务是「阶段3 单图 C++ PoC」**：须等阶段2 闭合（bmodel 到位）。注意 `best.onnx` 输出不含 NMS，后处理（xywh->xyxy、NMS、rescale）须在 C++ 侧实现。参考 `tools/image_inference/bmrt_load_test.cpp` 的 BMRuntime 加载范式。
- **若任务是「其他/探索」**：先读 `docs/PROGRESS.md` 第 4 节「当前阻塞与待办」，确认前置条件满足再动手。不满足时不要伪造进度。

### 6. 验证与提交
- 改动后先跑测试：`python3 tests/run_tests.py`（须 26 项全过）。
- 敏感信息扫描：`python3 tools/redact_secrets.py --scan hangzhouwan_beishang/`。
- 仅本地 `git commit`，**禁止 `git push`**。提交信息用中文、conventional 风格（chore/test/docs/feat/fix）。
- 改动文档/进度后同步更新 `docs/PROGRESS.md` 的「最后更新」与阶段状态。

### 7. 红线（违反即破坏项目约束）
- 不得在工控机装 TPU-MLIR / torch / CUDA / SAIL / onnxruntime-gpu。
- 不得在工控机转换 bmodel。
- 不得连正式 RTSP/MQTT/RTMP、不得改 systemd、不得重写历史、不得 push、不得提交真实凭据。
- 不得运行 `starter_optimized.py` / `start_optimized.bat`。
- 不得把「文件存在」写成「已验证可用」，不得伪造推理结果或测试结果。
- 目标芯片恒为 BM1684，模型路线恒为 F32->INT8（无 FP16）。
