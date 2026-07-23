# 仓库依赖与文件分类审计

> 审计日期：2026-07-23
> 审计基线：Git HEAD `662dac5`（分支 `feat/bm1684-edge-deployment`）
> 审计范围：仓库全部 332 个 Git 跟踪文件 + 本地未跟踪资产

## 一、审计方法

对仓库内所有主要目录、根目录文件和较大资产，逐一检查以下引用来源：

- `CMakeLists.txt` 及子目录构建文件
- `tools/release/` 下所有发布脚本
- `deploy/` 下安装、systemd、tmpfiles 脚本
- Python import 与路径引用
- Shell 脚本路径引用
- 配置文件（`config/`、`.env.example`）
- 单元测试和集成测试
- 文档命令
- Release 打包清单（`build_release.sh` 引用的文件）
- 模型加载代码（`services/business_enrichment/config.py`、C++ 源码）
- 测试数据读取代码
- Git tracked/untracked 状态

## 二、分类统计

| 分类 | 文件数 | 说明 |
|---|---|---|
| KEEP | 176 | 正式生产代码、构建配置、测试、部署脚本、配置模板、当前有效文档 |
| MOVE | 0 | 本轮无需移动的文件（docs 内部重组在 Phase 4 处理） |
| ARCHIVE | 0 | 历史材料保留在 docs/ 中，部分移入 docs/history/ |
| REMOVE | 156 | 确认无生产引用，可由 Git 历史恢复 |
| GENERATED | 0 (tracked) | 无生成物被跟踪；本地生成物已在 .gitignore 中 |
| EXTERNAL | 4 (untracked) | 模型权重文件，被 .gitignore 排除，本地存在 |
| UNKNOWN | 0 | 所有文件均已分类 |

REMOVE 明细：nginx 1.7.11.3 Gryphon/ (125 files, 1.8M) + hangzhouwan_beishang/ (30 files, 9.5M) + yolov7_requirements.txt (1 file, 6.9K) = 156 files, ~11.3M

EXTERNAL 明细（未跟踪，本地存在）：weights/0121_random_forest_model.pkl (7.6M), weights/beishang_x-l.pkl (2.1M), weights/best.onnx (24.1M), weights/beet0110.pt (74.8M, 开发用)

## 三、详细审计表

| 路径 | 当前用途 | 引用位置 | 分类 | 处理建议 | 风险 | 验证方式 |
|---|---|---|---|---|---|---|
| CMakeLists.txt | C++ 构建配置 | 自身 | KEEP | 保留 | 无 | cmake + CTest |
| README.md | 项目入口文档 | 文档系统 | KEEP | 更新过期路径引用 | 低 | 文档一致性检查 |
| .env.example | 环境变量模板 | deploy/、services/ | KEEP | 保留 | 无 | env 解析 |
| .gitignore | 忽略规则 | Git | KEEP | 移除 beishang/nginx 专用规则 | 低 | git status |
| yolov7_requirements.txt | Windows conda freeze (UTF-16) | 仅 docs/03、requirements/README.md | REMOVE | 删除 | 无 | 引用检查 |
| artifacts/bm1684-f32/*.bmodel | 生产推理模型 | build_release.sh、config.py、C++ | KEEP | 保留 | 无 | SHA256 + preflight |
| config/ | 配置模板 | build_release.sh、测试 | KEEP | 保留 | 无 | YAML 解析 |
| deploy/ | 安装/systemd/tmpfiles/preflight/rollback | systemd、hzwctl.py、测试 | KEEP | 保留 | 无 | bash -n + systemd-analyze |
| docs/00-index.md | 文档导航索引 | 文档系统 | KEEP | 更新导航 | 低 | 一致性检查 |
| docs/01~27-*.md | 阶段1-4过程报告 | docs/00-index.md | KEEP | 移入 docs/history/ | 低 | 一致性检查 |
| docs/PROGRESS.md | 阶段进度索引 | docs/00-index.md | KEEP | 更新测试数量 | 低 | 一致性检查 |
| docs/production/ | 生产运维文档 | 文档系统 | KEEP | 保留 | 低 | 一致性检查 |
| hangzhouwan_beishang/ | Windows Python 原型 | C++注释、测试、文档、.gitignore | REMOVE | 删除；更新引用 | 中 | 见详细分析 |
| include/ | C++ 头文件 (21个) | CMakeLists.txt、src/ | KEEP | 保留；更新注释 | 无 | CMake 构建 |
| nginx 1.7.11.3 Gryphon/ | Windows nginx-rtmp 分发包 | .gitignore、docs、test_assets.py(排除) | REMOVE | 删除 | 低 | 见详细分析 |
| requirements/ | 依赖分类说明和清单 | README.md、文档 | KEEP | 保留 | 无 | - |
| services/business_enrichment/ | Business 富化 Python 服务 | build_release.sh、systemd、测试 | KEEP | 保留 | 无 | pytest + preflight |
| src/ | C++ 生产源码 (16个) | CMakeLists.txt | KEEP | 保留 | 无 | CMake + CTest |
| testdata/test.mp4 | 测试视频 (16M) | test_assets.py、工具、测试 | KEEP | 保留 | 无 | ffprobe + 测试 |
| tests/ | 测试套件 (31个) | CI、开发流程 | KEEP | 保留；更新路径引用 | 低 | pytest + CTest |
| tools/ | 开发工具 (35个) | 构建发布流程 | KEEP | 保留 | 无 | bash -n + 构建 |
| weights/ (未跟踪) | 模型权重文件 | build_release.sh、config.py | EXTERNAL | 保留；完善 manifest | 无 | preflight SHA256 |

## 四、hangzhouwan_beishang/ 删除条件检查

| 条件 | 满足 | 证据 |
|---|---|---|
| CMake 无引用 | 是 | CMakeLists.txt 不引用 |
| Release 打包无引用 | 是 | build_release.sh 不引用 |
| 生产代码无引用 | 是 | grep 确认无 import；C++ 仅注释 |
| 测试无引用 | 需更新 | test_assets.py 检查 beishang/weights/ 路径 |
| 工具脚本无引用 | 需更新 | README 引用 redact_secrets --scan beishang/ |
| 文档不作为操作入口 | 需更新 | 多个文档引用 |
| baseline Release 成功 | 是 | baseline-662dac5 30/30 |
| Git 历史可恢复 | 是 | 全部在 Git 中 |
| 删除后构建测试通过 | 待验证 | 需先更新测试和文档 |

需更新的引用：test_assets.py (WEIGHTS路径、字体检查)、test_onnx_audit.py (ONNX路径)、manifest.example.yaml、README.md、C++头文件注释、.gitignore、requirements/README.md、docs/多个文件

## 五、nginx 1.7.11.3 Gryphon/ 删除条件检查

| 条件 | 满足 | 证据 |
|---|---|---|
| CMake 无引用 | 是 | - |
| Release 打包无引用 | 是 | - |
| 生产代码无引用 | 是 | - |
| 测试无引用 | 是 | test_assets.py 仅用 nginx 作排除过滤器 |
| 工具脚本无引用 | 是 | - |
| 文档不作为操作入口 | 是 | 仅历史文档提及 |
| baseline Release 成功 | 是 | - |
| Git 历史可恢复 | 是 | - |
| 删除后构建测试通过 | 是 | nginx 不参与构建或测试 |

nginx 目录可直接删除，无需更新代码或测试。test_assets.py 中的 nginx 过滤器变为无操作但不导致失败。

## 六、关键发现

1. weights/ 路径不一致：build_release.sh 和 config.py 引用根 weights/，但 test_assets.py 和 test_onnx_audit.py 检查 hangzhouwan_beishang/weights/。为历史迁移遗留。

2. ONNX 测试跳过原因：test_onnx_audit.py 检查 hangzhouwan_beishang/weights/best.onnx (不存在)，实际文件在 weights/best.onnx。inspect_onnx.py 仅依赖标准库。修正路径后测试将运行并通过。

3. simhei.ttf 非生产依赖：生产 C++ 使用 src/image_io/jpeg_io.cpp 内置 5x7 位图字体，不加载 TTF。simhei.ttf 仅被 Windows 原型使用。

4. artifacts/ 已在 .gitignore 但 bmodel 仍跟踪：bmodel 在规则前已提交，预期行为。

5. 生产 Release sha256sum 不一致 (预检发现)：当前生产 Release bin/dual_stream_app SHA256 与记录不匹配，为预存问题，本轮不处理。

## 七、风险评估

| 风险项 | 等级 | 缓解措施 |
|---|---|---|
| 删除 beishang 后测试失败 | 中 | 先更新测试引用，验证通过后再删除 |
| 文档命令过期 | 低 | 同步更新所有文档路径引用 |
| weights/ 外部资产丢失 | 低 | 完善 manifest，记录 SHA256 和来源 |
| ONNX 测试从 skip 变为 pass | 低 | 记录基线变化，报告中说明 |
