# 00 · 文档导航索引

> 导航：[项目 README](../README.md) · [进度跟踪](PROGRESS.md) · [Agent 接手提示词](AGENT_HANDOFF.md)

新开发者阅读顺序：**README -> PROGRESS -> 本索引 -> production/operations-guide**。

## 当前有效文档

| 文档 | 用途 | 状态 |
|---|---|---|
| [README.md](../README.md) | 项目入口：概述、快速开始、目录、约束 | ✅ |
| [PROGRESS.md](PROGRESS.md) | 阶段进度唯一权威索引、阻塞、ADR | ✅ |
| [AGENT_HANDOFF.md](AGENT_HANDOFF.md) | Agent 接手提示词 | ✅ |
| [credential-rotation-checklist.md](credential-rotation-checklist.md) | 凭据轮换清单（RTSP/MQTT/API Token） | ✅ |

## 架构文档

| 文档 | 用途 |
|---|---|
| [architecture/repository-layout.md](architecture/repository-layout.md) | 仓库目录结构与职责说明 |
| [architecture/repository-cleanup-audit.md](architecture/repository-cleanup-audit.md) | 仓库依赖与文件分类审计报告 |

## 生产运维文档

| 文档 | 用途 | 状态 |
|---|---|---|
| [production/operations-guide.md](production/operations-guide.md) | 生产运维手册（启动/停止/状态/日志/配置/升级/回滚/故障） | ✅ |
| [production/stage7-validation-guide.md](production/stage7-validation-guide.md) | 阶段7实装验证报告（T1-T10方法+结果） | ✅ |
| [production/manual-long-run-guide.md](production/manual-long-run-guide.md) | 人工长时测试指南（L1-L4） | ✅ |
| [production/windows-replacement-plan.md](production/windows-replacement-plan.md) | Windows服务灰度替换与回滚方案 | ✅ |
| [production/project-directory-migration.md](production/project-directory-migration.md) | 项目目录迁移记录 | ✅ |

## 历史阶段文档

阶段1-4过程报告已移至 [`history/`](history/) 目录，保留可追溯性，不再作为当前操作入口。

| 阶段 | 文档 |
|---|---|
| 阶段1 | `history/01-migration-gap-analysis.md`、`history/03-project-assets-inventory.md`、`history/04-stage1-full-project-baseline.md`、`history/05-model-and-test-assets-status.md` |
| 阶段2 | `history/06-stage2-onnx-audit-and-bmodel.md`、`history/12-x86-server-check.md`、`history/13-bm1684-f32-conversion-report.md`、`history/14-bm1684-f32-delivery-guide.md` |
| 阶段2B | `history/15-stage2b-board-model-validation.md` |
| 阶段3 | `history/16-stage3-single-image-cpp-poc.md` |
| 阶段4 | `history/17-stage4-single-video-hardware-pipeline.md` 至 `history/24-stage4-3-manual-long-run-test.md` |
| 双路/AIS | `history/25-dual-stream-full-stack.md`、`history/26-dual-stream-manual-long-run.md`、`history/27-coordinate-ais-mqtt-architecture.md` |

## 工具与测试导航

| 路径 | 用途 |
|---|---|
| `tests/run_tests.py` | 测试入口（纯标准库，`python3 tests/run_tests.py`） |
| `python3 -m pytest tests/` | pytest 全量测试 |
| `cd build && ctest --output-on-failure` | C++ 单元测试 |
| `tools/inspect_onnx.py` | ONNX 审计（纯标准库 protobuf 解析） |
| `tools/redact_secrets.py` | 敏感信息脱敏/扫描（含 `--scan` 仓库扫描） |
| `tools/validate_config.py` | 配置校验 |
| `tools/convert_model/README.md` | bmodel 转换说明（x86） |
| `tools/release/build_release.sh` | Release 构建脚本 |
| `tools/release/verify_release.sh` | Release 验证脚本 |
| `config/README.md` | 配置模板说明 |
| `requirements/README.md` | 依赖分类说明 |
