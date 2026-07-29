# 05 · 模型与测试资产状态结论

> 阶段1门禁判定依据。所有结论基于本地完整 Git 克隆的真实检查，不臆测、不沿用旧报告。

## 1. 逐项回答

| 问题 | 结论 | 证据 |
|---|---|---|
| `best.onnx` 是否存在 | **否** | `find` 无结果；`.gitignore` 排除 `*.onnx` |
| 两个坐标模型是否存在 | **否**（`0121_random_forest_model.pkl` 与 `beishang_x-l.pkl` 均缺失） | `find` 无结果；`.gitignore` 排除 `*.pkl` |
| 字体是否存在 | **是** | `weights/simhei.ttf`，9.4M，SHA256 见 03 |
| 是否有离线测试图片 | **否**（仅 nginx 模块自带 bg.jpg，非项目资产） | `find` + 集成测试 `test_no_project_test_image` |
| 是否有离线测试视频 | **是** | `testdata/test.mp4`，H264 960×544@20fps，Sophon-FFmpeg 可解码 |
| 是否可以检查 ONNX 结构 | **否**（工控机未装 onnx；且模型缺失） | 见 03 §5 |
| 是否可以生成旧模型基线 | **否**（缺模型 + 缺 onnxruntime/torch） | 工控机 Python 仅有 numpy/yaml/psutil |
| 是否需要在 x86 环境补充工作 | **是** | 补齐三个模型文件；x86 复现 ONNX/坐标模型基线 |
| 是否具备进入 bmodel 转换阶段 | **否** | 见下 §2 阻塞项 |

> 严格区分：“文件存在”不等于“已验证可用”。字体与测试视频已验证可用；
> 三个模型为“缺失”，不存在“可用”与否的判定。

## 2. 当前阻塞项

1. **检测模型 `best.onnx` 缺失** — Stage2 转 bmodel 的前置资产，必须从原 Windows 机器或 x86 训练环境补齐并入库（或置于 `/data/hangzhouwan/weights/`）。
2. **两个坐标随机森林 `.pkl` 缺失** — A/B 路坐标映射依赖，需补齐并在 x86 验证加载与输入输出形状。
3. **x86 模型转换工具链未就位** — TPU-MLIR / bm_model 转换须在 x86 开发机完成（工控机为 SoC，按计划不装转换工具链）。
4. **工控机 Python 推理栈缺失** — `onnx`/`onnxruntime`/`joblib`/`scikit-learn` 未装；本阶段按计划不装，旧模型基线须在 x86 完成。

## 3. 已就绪项（可进入下一阶段准备工作）

- BM1684 运行时已就绪：libsophon 0.4.9 LTS、`libbmrt.so.1.0`、BMCV/BMVideo/BMVPU、Sophon-FFmpeg/OpenCV 0.8.0、`h264_bm`/`h265_bm` 硬编解码、C++ 编译环境完整（阶段0已确认）。
- 配置/脱敏/校验/测试框架已建立（阶段1），示例配置无敏感值，离线测试 24 项全部通过。
- 源码已解除对明文凭据、Windows 路径、固定生产地址的依赖。

## 4. 阶段门禁结论

**阶段部分通过，需要补充模型或测试资产。**

- 项目基础（Git 分支、安全配置、中文日志、脱敏、配置校验、离线测试、依赖分类）已完成；
- 但三个模型文件缺失，**不具备直接进入 bmodel 转换阶段的条件**；
- 待在 x86 / 原 Windows 机器补齐 `best.onnx` 与两个 `.pkl`，并完成 ONNX 结构审计与坐标模型加载验证后，方可进入 Stage2（ONNX 审计与 bmodel 转换）。

---

## 阶段2 更新（2026-07-21）：模型已补齐

三个模型已补齐至 `weights/`，阶段1“缺失”结论更新为“存在”：

| 资产 | 阶段1状态 | 阶段2状态 |
|---|---|---|
| `best.onnx` | 缺失 | **存在·已审计**（见 `docs/06` §3） |
| `0121_random_forest_model.pkl` | 缺失 | 存在（坐标迁移属阶段5） |
| `beishang_x-l.pkl` | 缺失 | 存在（坐标迁移属阶段5） |

- ONNX 审计完成：动态输入 `[batch,3,height,width]` 须固化 `[1,3,640,640]`，输出 `output` 不含 NMS；
- INT8 校准集就绪（63 帧）；x86 转换脚本就绪；板端最小 C++ 加载程序编译并验证 API 链路；
- bmodel 转换为 x86-only，待 x86 执行 `tools/convert_model/convert_bmodel.sh` 后板端加载/精度验证。
- 详细结论见 `docs/06-stage2-onnx-audit-and-bmodel.md`。
