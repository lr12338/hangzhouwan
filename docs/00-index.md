# 00 · 文档导航索引

> 导航：[项目 README](../README.md) · [进度跟踪](PROGRESS.md) · [Agent 接手提示](AGENT_HANDOFF.md)

新开发者阅读顺序：**README -> PROGRESS -> 本索引 -> production/operations-guide**。

## 当前有效文档

| 文档 | 用途 |
|---|---|
| [README.md](../README.md) | 项目入口：概述、快速开始、目录、约束 |
| [PROGRESS.md](PROGRESS.md) | 当前生产状态唯一权威索引、修复记录、待完成项 |
| [AGENT_HANDOFF.md](AGENT_HANDOFF.md) | Agent 接手提示词（快速操作、约束、近期变更） |
| [credential-rotation-checklist.md](credential-rotation-checklist.md) | 凭据轮换清单（RTSP/MQTT/API Token） |

## 架构文档

| 文档 | 用途 |
|---|---|
| [architecture/repository-layout.md](architecture/repository-layout.md) | 仓库目录结构与职责说明 |
| [architecture/repository-cleanup-audit.md](architecture/repository-cleanup-audit.md) | 仓库依赖与文件分类审计报告 |

## 生产运维文档（活跃）

| 文档 | 用途 |
|---|---|
| [production/operations-guide.md](production/operations-guide.md) | 生产运维手册（启动/停止/状态/日志/配置/升级/回滚/故障） |
| [production/maintenance-window-runbook.md](production/maintenance-window-runbook.md) | VPU 重连修复维护窗口 Runbook（门禁/步骤/回滚） |
| [production/maintenance-window-audit-evidence.md](production/maintenance-window-audit-evidence.md) | 生产部署审计证据（1eba419/9d449ab/c008eac 部署记录） |
| [production/manual-long-run-guide.md](production/manual-long-run-guide.md) | 人工长时测试指南（L1-L4） |

## 历史文档

开发阶段过程报告和已完成的方案文档已移至 [`history/`](history/) 目录，保留可追溯性，不再作为当前操作入口。

| 类别 | 文档 |
|---|---|
| 阶段 1-4 | `history/01` ~ `history/24`（迁移/审计/模型/管线/稳定性） |
| 双路/AIS | `history/25` ~ `history/27`（双路全栈/长测/AIS 架构） |
| 已完成方案 | `history/28`（Windows 替换）、`history/29`（目录迁移）、`history/30`（阶段7验证）、`history/31`（VPU 修复候选） |

## 工具与测试导航

| 路径 | 用途 |
|---|---|
| `python3 -m pytest tests/ -q` | Python 全量测试（159 passed） |
| `cd build && ctest --output-on-failure` | C++ 单元测试（16/16 passed） |
| `tests/run_tests.py` | 测试入口（纯标准库） |
| `tools/release/build_release.sh` | Release 构建 |
| `tools/release/verify_release.sh` | Release 验证 |
| `tools/release/activate_release.sh` | Release 激活（root 执行） |
| `tools/release/rollback_release.sh` | Release 回滚 |
| `tools/hzwctl.py` | 运维控制工具（status/health/preflight/smoke） |
| `tools/validate_config.py` | 配置校验 |
| `tools/redact_secrets.py` | 敏感信息脱敏/扫描 |
| `config/README.md` | 配置模板说明 |
| `requirements/README.md` | 依赖分类说明 |
