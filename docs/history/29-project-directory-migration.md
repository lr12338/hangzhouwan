# 项目目录迁移文档

## 1. 迁移背景

杭州湾 BM1684 边缘工控机生产环境目录治理与无损迁移。原源码仓库位于
`/home/linaro/hangzhouwan-orign/hangzhouwan`，需统一到
`/home/linaro/hangzhouwan`，同时保持生产服务不中断。

迁移前生产状态：双路正式推流正常，Business 和 Video 服务均为 active，
健康状态 HEALTHY，AIS/MQTT 链路正常。

## 2. 原路径与新路径

| 项目 | 原路径 | 新路径 |
|---|---|---|
| 源码仓库 | `/home/linaro/hangzhouwan-orign/hangzhouwan` | `/home/linaro/hangzhouwan` |
| 兼容软链接 | （不存在） | `/home/linaro/hangzhouwan-orign/hangzhouwan` -> `/home/linaro/hangzhouwan` |

## 3. 路径职责划分

| 路径 | 类型 | 职责 |
|---|---|---|
| `/home/linaro/hangzhouwan` | 源码仓库 | Git 工作区，分支 `feat/bm1684-edge-deployment`，用于构建 Release 和开发 |
| `/etc/hangzhouwan/` | 生产配置 | `application.yaml`、`business.env`、`video.env`（权限 640 root:linaro） |
| `/run/hangzhouwan/` | 运行时 | `business.sock`、`video-health.sock`（tmpfiles.d 管理） |
| `/opt/hangzhouwan/current` | Release | 当前激活版本软链接，指向 `/opt/hangzhouwan/releases/<version>-<commit>/` |
| `/opt/hangzhouwan/releases/` | Release 制品 | 不可变发布目录，含 bin、models、services、venv、systemd |
| `/var/log/hangzhouwan/` | 日志 | 诊断包和日志输出目录 |
| `/var/lib/hangzhouwan/` | 数据 | JSONL 事件输出和状态数据 |

> 生产服务（systemd unit、ExecStart、WorkingDirectory）完全依赖 `/opt/hangzhouwan/current`
> 和 `/etc/hangzhouwan/`，与源码仓库路径零耦合。迁移源码目录不影响生产运行。

## 4. 迁移步骤

1. **只读审计**：检查 Git 状态（分支 `feat/bm1684-edge-deployment`，提交 `efe1d1a`，工作区干净），
   审计 systemd unit、进程 cwd/exe/cmdline、`/etc/hangzhouwan/`、`/opt/hangzhouwan/`，
   确认生产服务与源码路径零耦合。
2. **创建新目录**：通过 `git clone --no-local` 从旧仓库创建独立副本到 `/home/linaro/hangzhouwan`，
   保留完整 `.git`、分支、remote 和提交历史。
3. **迁移本地资产**：复制 `weights/`（模型文件）、`testdata/calibration/`（校准帧）、
   `testdata/model_test/`（测试数据）到新目录。
4. **更新路径引用**：将 8 个文档文件中的 `/home/linaro/hangzhouwan-orign/hangzhouwan`
   替换为 `/home/linaro/hangzhouwan`。
5. **整理 .gitignore**：新增 `.pytest_cache/`、`*.sock`、`*.pid`、`"rtmp:"` 规则。
6. **创建兼容软链接**：将旧目录移动到隔离区，创建软链接
   `/home/linaro/hangzhouwan-orign/hangzhouwan` -> `/home/linaro/hangzhouwan`。
7. **隔离历史目录**：将 `/home/linaro/` 下的历史下载、压缩包和误创建文件移动到隔离目录。
8. **Git 提交**：提交路径引用更新、.gitignore 整理和迁移文档。

## 5. 兼容软链接

```
/home/linaro/hangzhouwan-orign/hangzhouwan -> /home/linaro/hangzhouwan
```

创建前已确认：
- 旧目录已完整迁移到隔离目录（非删除）；
- 软链接不会覆盖真实目录；
- systemd、脚本和相对路径均可正常解析；
- 回滚时只需删除软链接并恢复旧目录。

## 6. 验证命令

```bash
# 源码仓库验证
cd /home/linaro/hangzhouwan
git status --short --branch
git remote -v
git log -1 --oneline

# 旧路径引用检查（应仅剩测试脚本中的验证检查）
grep -rn 'hangzhouwan-orign' --include='*.py' --include='*.sh' --include='*.yaml' --include='*.md' .

# 兼容软链接验证
ls -la /home/linaro/hangzhouwan-orign/hangzhouwan
readlink -f /home/linaro/hangzhouwan-orign/hangzhouwan

# 生产服务验证
systemctl is-active hangzhouwan-business
systemctl is-active hangzhouwan-video
systemctl status hangzhouwan-business --no-pager
systemctl status hangzhouwan-video --no-pager
journalctl -u hangzhouwan-business -n 50 --no-pager
journalctl -u hangzhouwan-video -n 50 --no-pager
```

## 7. 回滚步骤

如果迁移后出现问题，按以下逐项恢复方案回滚。每一步在执行前检查目标是否已存在，
如遇冲突立即停止，不要强制覆盖。

隔离目录时间戳：`20260723-101304`

### 步骤 1：删除兼容软链接（仅当目标是软链接时）

```bash
if [ -L /home/linaro/hangzhouwan-orign/hangzhouwan ]; then
  rm /home/linaro/hangzhouwan-orign/hangzhouwan
  echo "已删除软链接"
else
  echo "目标不是软链接，停止回滚" >&2
  exit 1
fi
```

### 步骤 2：恢复旧源码目录（冲突即停止）

```bash
QDIR=/home/linaro/hangzhouwan_migration_backup/20260723-101304
TARGET=/home/linaro/hangzhouwan-orign/hangzhouwan

if [ -e "$TARGET" ]; then
  echo "目标已存在，停止回滚：$TARGET" >&2
  exit 1
fi
if [ ! -d "$QDIR/hangzhouwan" ]; then
  echo "隔离目录中无源码，停止回滚" >&2
  exit 1
fi
mv "$QDIR/hangzhouwan" "$TARGET"
echo "已恢复源码目录到 $TARGET"
```

### 步骤 3：恢复旧 .git（冲突即停止）

```bash
if [ -e /home/linaro/hangzhouwan-orign/.git ]; then
  echo "目标已存在，跳过"
else
  mv "$QDIR/hangzhouwan-orign_dotgit" /home/linaro/hangzhouwan-orign/.git
  echo "已恢复 .git"
fi
```

### 步骤 4：恢复旧 artifacts（冲突即停止）

```bash
if [ -e /home/linaro/hangzhouwan-orign/artifacts ]; then
  echo "目标已存在，跳过"
else
  mv "$QDIR/hangzhouwan-orign_artifacts" /home/linaro/hangzhouwan-orign/artifacts
  echo "已恢复 artifacts"
fi
```

### 步骤 5：恢复历史文件（逐项检查，冲突即跳过）

```bash
for item in hangzhouwan-main hangzhouwan_src hangzhouwan_conversation_export \
            hangzhouwan_migration_audit hz.tar.gz hz_full.tar.gz \
            fetch_repo.py fetch2.py hz_dl.log "udo ss -lntup"; do
  if [ -e "/home/linaro/$item" ]; then
    echo "跳过（已存在）：/home/linaro/$item"
  elif [ -e "$QDIR/home_linaro/$item" ]; then
    mv "$QDIR/home_linaro/$item" "/home/linaro/$item"
    echo "已恢复：/home/linaro/$item"
  else
    echo "隔离目录中不存在：$item"
  fi
done
```

### 步骤 6：验证

```bash
cd /home/linaro/hangzhouwan-orign/hangzhouwan
git status --short --branch
git log -1 --oneline

# 生产服务无需重启（服务运行在 /opt/hangzhouwan/current，与源码路径无关）
systemctl is-active hangzhouwan-business
systemctl is-active hangzhouwan-video
```

> 回滚不需要重启生产服务，因为 systemd unit 的 WorkingDirectory 和 ExecStart
> 均指向 `/opt/hangzhouwan/current`，与源码目录无关。

## 8. 隔离目录

隔离目录位置：`/home/linaro/hangzhouwan_migration_backup/20260723-101304/`

隔离内容包括：
- `hangzhouwan/`：旧源码仓库完整副本（含 .git、build、Testing、logs 等）
- `hangzhouwan-orign_dotgit/`：旧仓库父目录的空 .git（无提交）
- `hangzhouwan-orign_artifacts/`：旧仓库父目录的 artifacts（stage4 测试日志）
- `home_linaro/`：`/home/linaro/` 下的历史文件（hangzhouwan-main、hangzhouwan_src、
  hz.tar.gz、hz_full.tar.gz、fetch_repo.py、fetch2.py、hz_dl.log、udo ss -lntup 等）

隔离前后已生成文件清单和总大小记录。

## 9. 待人工确认删除的历史文件

以下文件已隔离但暂不删除，需在满足以下全部条件后由人工确认删除：
- 新路径构建通过；
- 服务运行正常；
- 完成一次受控重启；
- 完成 Release 回滚验证；
- 稳定观察期通过（建议 >= 7 天）；
- Git 与模型资产完整；
- 用户明确确认。

| 隔离文件 | 原路径 | 隔离原因 |
|---|---|---|
| `hangzhouwan/` | `/home/linaro/hangzhouwan-orign/hangzhouwan` | 已被新目录和软链接替代 |
| `hangzhouwan-orign_dotgit/` | `/home/linaro/hangzhouwan-orign/.git` | 空仓库（无提交），git init 残留 |
| `hangzhouwan-orign_artifacts/` | `/home/linaro/hangzhouwan-orign/artifacts` | stage4 测试日志，已归档 |
| `hangzhouwan-main/` | `/home/linaro/hangzhouwan-main` | GitHub main 分支下载，历史快照 |
| `hangzhouwan_src/` | `/home/linaro/hangzhouwan_src` | GitHub API 下载的文件子集 |
| `hz.tar.gz` | `/home/linaro/hz.tar.gz` | hangzhouwan-main 压缩包 |
| `hz_full.tar.gz` | `/home/linaro/hz_full.tar.gz` | hangzhouwan-main 完整压缩包 |
| `fetch_repo.py` | `/home/linaro/fetch_repo.py` | GitHub API 下载脚本 |
| `fetch2.py` | `/home/linaro/fetch2.py` | GitHub API 下载脚本 |
| `hz_dl.log` | `/home/linaro/hz_dl.log` | 空下载日志 |
| `udo ss -lntup` | `/home/linaro/udo ss -lntup` | 误创建文件（sudo ss -lntup 终端输出重定向） |

## 10. 执行记录

- 迁移时间：2026-07-23
- 迁移时间戳：20260723-101304
- 原提交：`efe1d1a`（生产部署：修复AIS/推理/健康接口，切换正式推流地址，更新全部运维文档）
- 新仓库分支：`feat/bm1684-edge-deployment`
- 新仓库 remote：`git@github.com:lr12338/hangzhouwan.git`
- 生产服务状态：迁移前后均为 active，无需重启
- 生产运行路径：未切换（服务始终运行在 `/opt/hangzhouwan/current`）
- 迁移提交 1：`4e604fc`（chore: 整理项目目录并统一生产运维路径）
- 迁移提交 2：`686cb20`（docs: 补充迁移提交哈希和验证结果）
- 收尾修正提交：见最终分支 HEAD（本次 .gitignore 修正 + 文档一致性更新）
- 最终分支 HEAD：`feat/bm1684-edge-deployment`（以 `git log -1 --oneline` 为准）
- 迁移后验证：Python 测试通过（以 `python3 -m pytest tests/` 实际输出为准），preflight 通过，生产服务 HEALTHY
- 测试命令：`python3 -m pytest tests/ -v`（以实际输出为准）
- 磁盘：根分区 5.8G 已用 73%（4.1G/5.8G），inode 19%（73236/393216）
- 新仓库大小：209M（含 .git 49M），隔离目录大小：467M
