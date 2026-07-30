# Agent 接手提示（AGENT_HANDOFF）

> 最后更新：2026-07-30

## 当前状态：生产部署运行中

| 项 | 值 |
|---|---|
| 仓库 | `https://github.com/lr12338/hangzhouwan.git` |
| 分支 | `feat/bm1684-edge-deployment` |
| 画面修复提交 | `ce0724b` |
| 本地路径 | `/home/linaro/hangzhouwan` |
| 平台 | BM1684-SOC（chipid `0x1684`），libsophon 0.4.9，sophon-ffmpeg 0.8.0 |
| 生产 Release | `video-frame-fix-20260730-9c6cec3`（`/opt/hangzhouwan/current`） |
| previous | `industrial-20260728-fdb878c` |
| 系统状态 | A 路 HEALTHY（~10fps），B 路 DEGRADED（摄像机 301 通道取流不稳） |

## 快速操作

```bash
# 状态
/opt/hangzhouwan/current/bin/hzwctl status
/opt/hangzhouwan/current/bin/hzwctl health

# 服务
sudo systemctl start hangzhouwan.target    # 启动（开机自启已 enable）
sudo systemctl stop hangzhouwan-video.service  # 仅停 Video
sudo systemctl restart hangzhouwan.target   # 重启全部

# 日志
journalctl -u hangzhouwan-video.service -f

# 构建+测试
cmake --build build -j && cd build && ctest --output-on-failure
python3 -m pytest -q

# 发布
bash tools/release/build_release.sh <name>
bash tools/release/verify_release.sh <release_dir>
sudo bash tools/release/activate_release.sh <release_dir>   # root 执行
sudo bash tools/release/rollback_release.sh                  # 回滚
```

## 关键约束

- 不得手工修改 `current`/`previous` 软链接，必须用 `activate_release.sh` / `rollback_release.sh`
- `activate_release.sh` 须 root 执行（`current`/`previous` 为 root 持有，单元文件 600）
- 不得在生产现场编译覆盖二进制；修复须走 构建->verify->activate 流程
- 不得执行 RTSP/RTMP/网络故障注入（仅维护窗口人工执行）
- 测试证据见 `docs/production/audit-evidence/`（.log 被 gitignore，仅磁盘保留）

## 近期变更

| 提交 | 内容 |
|---|---|
| `ce0724b` | 1280×720 缩放帧改为主机 AVFrame，修复 `h264_bm` 输入内存契约；已生产激活并通过 A/B 软件解码验证 |
| `2faf0ad` | 双路恢复与健康状态加固 |
| `4dffaa3` | docs: 长期稳定优化记录 |
| `c008eac` | perf: RTMP 重连日志节流（每10次汇总） |
| `9d449ab` | fix: RTMP muxer-only 重连 PTS 重置致推流自循环 |
| `1eba419` | fix: 健康判定 v2（重连宽限+防抖） |

## 待处理

- 画面修复 G8 已通过；继续执行 2-4h/24h 长稳观察
- B 路摄像机 301 通道取流不稳（`最近输入` 最大 80s 空档）-- 摄像机端问题
- RTMP 服务器双路并发丢连接（~15-18/min）-- 服务器/网络外部问题
- G1 真实 RTSP 100 轮 / G5 RTMP 100x / 长稳 2-4h -- 需维护窗口

详见 [PROGRESS.md](PROGRESS.md)、[production/video-corruption-fix.md](production/video-corruption-fix.md) 和 [production/maintenance-window-audit-evidence.md](production/maintenance-window-audit-evidence.md)。
