# 00 · 文档导航索引

> 导航：[项目 README](../README.md) · [进度跟踪](PROGRESS.md) · [Agent 接手提示词](AGENT_HANDOFF.md)

新开发者阅读顺序：**README -> PROGRESS -> 01 差距 -> 04/06 阶段报告**。

## 文档清单

| 编号 | 文档 | 用途 | 状态 |
|---|---|---|---|
| - | [README.md](../README.md) | 项目入口：概述、快速开始、目录、约束 | ✅ |
| - | [PROGRESS.md](PROGRESS.md) | 阶段进度唯一权威索引、阻塞、ADR | ✅ |
| 00 | 本文件 | 文档导航 | ✅ |
| 01 | [01-migration-gap-analysis.md](01-migration-gap-analysis.md) | 迁移差距分析，标注文件:行号 | ✅ 阶段1 |
| 03 | [03-project-assets-inventory.md](03-project-assets-inventory.md) | 项目资产清单（模型/字体/视频/源码） | ✅ 阶段1 |
| 04 | [04-stage1-full-project-baseline.md](04-stage1-full-project-baseline.md) | 阶段1 完整基线报告（16 节） | ✅ 阶段1 |
| 05 | [05-model-and-test-assets-status.md](05-model-and-test-assets-status.md) | 模型与测试资产状态结论（门禁依据） | ✅ 阶段1/2 |
| 06 | [06-stage2-onnx-audit-and-bmodel.md](06-stage2-onnx-audit-and-bmodel.md) | 阶段2 ONNX 审计与 bmodel 转换准备 | ✅ 阶段2 |
| - | [credential-rotation-checklist.md](credential-rotation-checklist.md) | 凭据轮换清单（RTSP/MQTT/API Token） | ✅ 阶段1 |
| - | [AGENT_HANDOFF.md](AGENT_HANDOFF.md) | 其他 Agent 快速接手的提示词 | ✅ |

> 编号 02（目标架构）与阶段0 系统审计见外部参考目录 `/home/linaro/hangzhouwan_src/docs/`（`00-current-system-audit.md`、`02-target-architecture.md`），不随仓库入库。

## 外部参考文档（不在本仓库）

| 路径 | 用途 |
|---|---|
| `/home/linaro/hangzhouwan_src/docs/00-current-system-audit.md` | 阶段0 现状系统审计 |
| `/home/linaro/hangzhouwan_src/docs/01-migration-gap-analysis.md` | 阶段0 迁移差距（早期版本） |
| `/home/linaro/hangzhouwan_src/docs/02-target-architecture.md` | 目标架构与 9 阶段路线 |
| `/home/linaro/hangzhouwan_src/docs/PROGRESS.md` | 阶段0 进度跟踪（早期版本） |

## 工具与测试导航

| 路径 | 用途 |
|---|---|
| `tests/run_tests.py` | 测试入口（26 项，纯标准库） |
| `tools/inspect_onnx.py` | ONNX 审计（纯标准库 protobuf 解析） |
| `tools/redact_secrets.py` | 敏感信息脱敏/扫描 |
| `tools/validate_config.py` | 配置校验 |
| `tools/convert_model/README.md` | bmodel 转换说明（x86） |
| `tools/image_inference/README.md` | 板端 C++ 推理说明 |
| `config/README.md` | 配置模板说明 |
| `requirements/README.md` | 依赖分类说明 |
