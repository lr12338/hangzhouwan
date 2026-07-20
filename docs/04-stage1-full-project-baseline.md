# 04 · 阶段1 完整项目基线报告

> 阶段1（BM1684 迁移·测试基线与安全配置）最终报告。依据本地完整 Git 克隆真实检查，
> 不连接正式 RTSP/MQTT/RTMP，不安装 SAIL/TPU-MLIR，不转换 bmodel，不改 systemd。

## 1. 阶段目标

1. 以本地完整 Git 仓库为唯一源码依据，重新核验项目、模型、测试数据与运行资源；
2. 建立独立 BM1684 迁移开发分支 `feat/bm1684-edge-deployment`；
3. 完成静态检查与离线测试；
4. 解除源码对明文凭据、Windows 路径、固定生产地址的依赖；
5. 建立中文日志、配置校验、敏感信息脱敏与测试框架；
6. 判断是否具备进入“ONNX 审计与 bmodel 转换阶段”的条件。

## 2. Git 工作区状态

| 项 | 内容 |
|---|---|
| 仓库根 | `/home/linaro/hangzhouwan-orign/hangzhouwan` |
| 远程 | `https://github.com/lr12338/hangzhouwan.git`（与目标一致） |
| 默认分支 | `main`（HEAD `56d380f3f25a49dfaa012946dd11d2c3a0fe7bef`） |
| 当前分支 | `feat/bm1684-edge-deployment`（由 `main` 创建，工作区原为干净） |
| 工作区 | 整改前干净；阶段1已本地提交 3 次，未 push |

## 3. 完整项目结构

业务代码集中于 `hangzhouwan_beishang/`（三入口、三流处理版本、两 AIS、两 config、
两 requirements、两套检测路径 PyTorch/ONNX，重复实现严重）。`nginx 1.7.11.3 Gryphon/`
为 Windows nginx-rtmp 源码树（与迁移无关，应剔除）。详见 `docs/03-project-assets-inventory.md`。

阶段1新增目录：`config/` `tools/` `tests/` `requirements/` `docs/`。

## 4. 模型与资源资产表

| 资产 | 状态 | SHA256 / 大小 |
|---|---|---|
| `best.onnx`（检测） | **缺失**（gitignore `*.onnx`） | - |
| `0121_random_forest_model.pkl`（A 路坐标） | **缺失**（gitignore `*.pkl`） | - |
| `beishang_x-l.pkl`（B 路坐标） | **缺失**（gitignore `*.pkl`） | - |
| `weights/simhei.ttf`（字体） | 存在·可用 | `aa4560dd…` / 9.4M |
| `testdata/test.mp4`（测试视频） | 存在·可用 | `e6ef38f3…` / 16M / H264 960×544@20fps |

完整资产表见 `docs/03`。三模型缺失经本地克隆真实确认，非沿用旧报告。

## 5. 原程序运行链路

生产入口 `starter_optimized.py`：创建单一 ONNX Runtime(CUDA) 检测器 -> 传给 A/B 两路
`OptimizedVideoStreamHandler`（三线程：cv2 软取流 / detect_and_annotate / FFmpeg libx264 软编推 RTMP）
-> `getais` MQTT 线程回调内同步 HTTP 查船名 -> `RUN_TIME=3600` 到点 `os.execv` 整进程重启。
逐项文件:行号见 `docs/01` 第 2 节。

## 6. 已确认问题及代码位置

详见 `docs/01-migration-gap-analysis.md`，均标注文件:行号。要点：
- 两路共享一个检测器，`img_width/img_height` 实例状态竞态（`detector.py:47,148`）；
- `libx264` 软编码、队列=3、逐帧 flush、FFmpeg 管道未消费（`stream_handler_optimized.py:100,32-33,303,137`）；
- 坐标模型逐框 `joblib.load` + 逐框 DataFrame（`find_ship.py:39,60,82,104,156`）；
- MQTT 回调内同步 HTTP 查船名、AIS 异常静默、全局字典绕锁直读（`getais.py:88,150`；`stream_handler_optimized.py:249`）；
- 逐框整帧 PIL 转换 + 逐框字体加载（`plot.py:30,32`）；
- 每小时整进程重启、无条件 kill 所有子进程（`starter_optimized.py:18,115-127,143`）。

> 上述架构级问题标注【重写】，阶段1不实施（不改算法）。

## 7. 安全配置整改

- 明文凭据（RTSP 账号密码 / RTMP 推流地址 / MQTT 账号密码 / 船名 usertoken / EZVIZ 令牌 / Agora 凭据）
  全部移出源码，改为环境变量注入；`streams` 默认 `disabled`。
- Windows 绝对路径（ffmpeg/字体/模型）全部改为仓库相对路径 + 环境变量覆盖。
- `except: pass` 改为受控中文日志（`getais.py` / `getais_flask.py`）。
- 新增 `config/application.example.yaml`（无真实敏感值）、`.env.example`、`config/README.md`。
- `.gitignore` 重写并补齐；80MB 含凭据产物 `output.txt` 已 untrack（本地保留）。

## 8. 中文日志改造

- 新增代码与工具（`tools/`、`tests/`）日志均为中文，格式 `时间 | 级别 | 模块 | 中文消息`。
- `getais.py` 异常由 `except: pass` 改为 `logging.warning("AIS消息处理异常 | 原因=%s", e)` 等。
- 脱敏工具 `tools/redact_secrets.py` 保证日志中不出现完整 RTSP/RTMP/Token/密码。

## 9. 离线测试结果

- 静态检查：`python3 -m compileall` 通过（exit 0），所有 `.py` 可编译。
- 测试套件：`python3 tests/run_tests.py` -> **24 项全部通过**（10 脱敏 + 10 配置校验 + 4 资产）。
- 测试视频：Sophon-FFmpeg `ffprobe` 解析 H264 960×544@20fps 成功，硬件解码路径激活。
- 未执行任何正式 RTSP/MQTT/RTMP 连接；未伪造推理结果。

## 10. 敏感信息扫描结果

整改前命中（类型 / 原位置 / 是否已整改）：

| 敏感类型 | 原位置（整改前） | 已整改 |
|---|---|---|
| 摄像机 RTSP 账号密码 | `utils_demo/config.py:9,26` 等 | 是 |
| RTMP 推流地址 | `utils_demo/config.py:12,29` 等 | 是 |
| EZVIZ 开放令牌 | `config.py:10-11,27-28` / `demo.py:476,482` / `detect.py:172` | 是 |
| MQTT 账号密码 | `getais.py:24-26` / `getais_flask.py:25-27` | 是 |
| 船名 usertoken | `getais.py:152` / `getais_flask.py:170` | 是 |
| Agora token/APPID/证书 | `method.py:6-8` | 是 |
| Windows 绝对路径 | `config.py` / `find_ship.py` / `demo.py` / `detect.py` / `stream_handler_v2.py` | 是 |
| 嵌入产物的推流地址 | `output.txt`（已 untrack） | 是 |

> 整改后 `git grep` 在当前分支源码 `*.py/*.md/*.bat/*.txt/*.yaml` 中 **0 命中**真实凭据。
> 测试夹具已改用合成值（TEST-NET `192.0.2.1` / `example.com`），不含真实凭据。
> 历史泄露（初始提交 + output.txt blob）见 `docs/credential-rotation-checklist.md`，
> 凭据视为已泄露，须轮换（不重写历史）。

## 11. 依赖分类

原 `requirements*.txt` 冲突 pin 不可用。新增 `requirements/`：
- `baseline-x86.txt`：x86 复现 ONNX/坐标模型基线（onnx/onnxruntime/numpy/opencv/joblib/sklearn/pandas/Pillow），**不在工控机装**；
- `tools.txt`：配置校验/脱敏/测试（PyYAML/jsonschema/pytest，可选）；
- BM1684 生产：C++ 全栈（BMRuntime/BMCV/Sophon-FFmpeg/Sophon-OpenCV/MQTT C++/yaml-cpp），**禁止 torch/CUDA/onnxruntime-gpu**。
- 工控机实测：Python 3.8.2，仅 numpy/PyYAML/psutil；离线测试基于 stdlib unittest，无需 pip。

## 12. 修改文件清单

| 文件 | 变更 |
|---|---|
| `.gitignore` | 重写（UTF-8 + 补齐忽略项） |
| `config/application.example.yaml` `logging.example.yaml` `README.md` | 新增 |
| `.env.example` | 新增 |
| `tools/redact_secrets.py` `validate_config.py` | 新增 |
| `requirements/baseline-x86.txt` `tools.txt` `README.md` | 新增 |
| `tests/**` | 新增（单测/集成/runner/fixtures/schemas） |
| `docs/01,03,04,05,credential-rotation-checklist` | 新增 |
| `hangzhouwan_beishang/config.py` `utils_demo/config.py` | 凭据/Windows 路径外置 |
| `hangzhouwan_beishang/utils_demo/getais.py` `getais_flask.py` | MQTT 凭据外置 + 中文日志 |
| `hangzhouwan_beishang/utils_demo/find_ship.py` `method.py` | Windows 路径外置 / Agora 凭据删除 |
| `hangzhouwan_beishang/demo.py` `detect.py` `stream_handler_v2.py` | Windows 路径/RTMP/EZVIZ 移除 |
| `hangzhouwan_beishang/优化部署指南.md` | 生产主机脱敏 |
| `hangzhouwan_beishang/output.txt` | untrack（80MB，含历史凭据） |

## 13. Git 提交情况

| # | SHA | 说明 |
|---|---|---|
| 1 | `20cf393` | chore: 建立BM1684安全配置和中文日志基础 |
| 2 | `6e0437f` | test: 增加配置脱敏与资产检查测试 |
| 3 | 本提交 | docs: 补充完整项目迁移基线和资产清单 |

- 分支：`feat/bm1684-edge-deployment`；未提交残留：无（docs 为本提交）。
- 模型/大文件：三模型缺失；`output.txt` 已 untrack（本地 80MB 保留，不入库）。
- 历史敏感信息：初始提交 `56d380f3` 含明文凭据（仓库 public），须轮换（见凭据清单），不重写历史。

## 14. 未通过项和阻塞项

- **阻塞**：`best.onnx` 与两个坐标 `.pkl` 缺失 -> 无法进入 bmodel 转换；
- **阻塞**：x86 模型转换工具链未就位（须在 x86 完成）；
- **待补**：x86 复现旧模型基线、ONNX 结构审计、坐标模型加载验证；
- 已完成：分支、安全配置、中文日志、脱敏、配置校验、离线测试（24 项通过）、依赖分类。

## 15. 回滚方法

```bash
cd /home/linaro/hangzhouwan-orign/hangzhouwan
# 回到阶段1起点（main）：
git switch main
# 或仅撤销阶段1三个提交但保留改动在工作区：
git reset --soft 56d380f3f25a49dfaa012946dd11d2c3a0fe7bef
# output.txt 在本地仍保留（仅从索引移除），如需恢复跟踪：
git checkout 56d380f3 -- hangzhouwan_beishang/output.txt
```

## 16. 阶段门禁结论

**阶段部分通过，需要补充模型或测试资产。**

项目基础（Git 分支、安全配置、中文日志、脱敏、配置校验、离线测试、依赖分类）已完成并
通过验收；但三个模型文件缺失，**不具备直接进入 ONNX 审计与 bmodel 转换阶段的条件**。
待在 x86 / 原 Windows 机器补齐 `best.onnx` 与两个 `.pkl`，并完成 ONNX 结构审计与坐标模型
加载验证后，方可进入 Stage2。
