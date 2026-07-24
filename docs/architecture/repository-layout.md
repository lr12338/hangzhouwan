# 仓库目录结构与职责说明

> 最后更新：2026-07-24（文档清理与生产稳定优化后）

## 顶层目录

| 目录/文件 | 用途 | 进入 Release | 允许生成物 | 维护责任 | 验证命令 |
|---|---|---|---|---|---|
| `CMakeLists.txt` | C++ 构建配置 | 否（构建用） | 否 | C++ 开发 | `cmake -S . -B build && cmake --build build -j4` |
| `README.md` | 项目入口文档 | 否 | 否 | 全员 | 文档一致性检查 |
| `.env.example` | 环境变量模板 | 否 | 否 | 运维 | env 解析 |
| `.gitignore` | Git 忽略规则 | 否 | 否 | 全员 | `git status` 干净 |
| `config/` | 配置模板（application/logging example） | 是（example.yaml） | 否 | 运维 | `python3 tools/validate_config.py config/application.example.yaml` |
| `deploy/` | 部署脚本、systemd unit、tmpfiles.d | 是（systemd/、tmpfiles.d/） | 否 | 运维 | `bash -n deploy/*.sh`、`systemd-analyze verify` |
| `include/` | C++ 头文件 | 否（编译用） | 否 | C++ 开发 | CMake 构建 |
| `src/` | C++ 生产源码 | 是（编译为 bin/dual_stream_app） | 否 | C++ 开发 | CMake 构建 + CTest |
| `services/` | Business 富化 Python 服务（AIS/坐标/MQTT） | 是 | 否（`__pycache__` 由 .gitignore 排除） | Python 开发 | `python3 -m pytest tests/` |
| `tools/` | 开发工具（release/deploy/ais/model/diagnostics/inference） | 部分（hzwctl、build_release） | 否 | 开发 | `bash -n tools/release/*.sh` |
| `tests/` | 测试套件（Python unit/integration + C++ unit_cpp） | 否 | 否（fixtures 显式管理） | 全员 | `python3 -m pytest tests/` + `cd build && ctest` |
| `testdata/` | 测试数据（test.mp4） | 否 | 否 | 测试 | `tests/integration/test_assets.py` |
| `artifacts/` | 模型资产（bmodel 入库） | 是（bmodel） | 否（stage4 等为 gitignore 生成物） | 模型运维 | preflight SHA256 |
| `weights/` | 模型权重文件（gitignore，不入库） | 是（build_release.sh 复制 PKL） | 否（外部资产） | 模型运维 | preflight SHA256 |
| `requirements/` | 依赖分类说明 | 否 | 否 | 开发 | - |
| `docs/` | 文档（架构/运维/历史） | 否 | 否 | 全员 | 文档一致性检查 |

## 已移除的历史内容

以下内容已从活跃分支移除，可通过 Git 历史追溯：

| 目录/文件 | 移除原因 | 最后可追溯提交 |
|---|---|---|
| `hangzhouwan_beishang/` | Windows Python 原型，生产 C++ 代码无依赖 | `5e336cc`（移除前）、`662dac5`（移除前 HEAD） |
| `nginx 1.7.11.3 Gryphon/` | Windows nginx-rtmp 分发包，生产使用 Sophon-FFmpeg | 同上 |
| `yolov7_requirements.txt` | Windows conda freeze，已由 `requirements/` 替代 | 同上 |

## 生产边界（不可变）

| 边界 | 路径 | 说明 |
|---|---|---|
| Git 源码 | `/home/linaro/hangzhouwan` | 本仓库 |
| 正式配置 | `/etc/hangzhouwan/` | application.yaml、business.env、video.env |
| 不可变 Release | `/opt/hangzhouwan/releases/<release-id>` | 每个 Release 独立目录 |
| 当前 Release | `/opt/hangzhouwan/current` | 指向当前生产 Release 的软链接 |
| 运行状态 | `/run/hangzhouwan/` | Unix Socket、PID 等 |
