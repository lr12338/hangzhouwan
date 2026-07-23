# Agent 接手提示（AGENT_HANDOFF）

## 当前状态：2026-07-23 生产部署运行中

项目根目录：`/home/linaro/hangzhouwan`
仓库：`https://github.com/lr12338/hangzhouwan`
分支：`feat/bm1684-edge-deployment`
当前 Release：`202607221953-3dfcf4e`（`/opt/hangzhouwan/current`，含热修补丁）
平台：BM1684-SOC（chipid `0x1684`，非 BM1684X），libsophon 0.4.9，sophon-ffmpeg 0.8.0

---

## 已完成

- **阶段7板端编译**：C++ 全量编译通过
- **systemd 实装**：Business/Video 单元已安装（未 enable），`systemd-analyze verify` 通过
- **Release 激活/回滚**：原子切换 + 自动回滚 + 60s smoke 验证通过
- **推流地址切换为正式地址**：已替代 Windows 旧服务，直接推流 `HangZhouBridgeNorth8_1` / `HangZhouBridgeNorth3_1`
- **AIS 订阅修复**：`upAIS/#` 通配符订阅（base_2250/2251 已离线），JSON 格式 AIS 数据解析支持
- **e2e_p95 健康接口修复**：新增 public 访问器，健康接口正确报告 e2e P95
- **sklearn 警告消除**：`delattr(feature_names_in_)` 修复
- **系统级优化**：journald 限制 50M，移除 Docker/Cursor Server/BSP 安装包
- **双路推流正常**：A/B 路 RTSP/RTMP 均连接，output_fps ~10，RTMP失败=0，重连=0
- **系统状态 HEALTHY**：business_state=COORD_ONLY，degradation=none

## 配置位置

| 文件 | 路径 | 说明 |
|---|---|---|
| 配置 | `/etc/hangzhouwan/application.yaml` | 唯一权威配置（640 root:linaro） |
| Business env | `/etc/hangzhouwan/business.env` | MQTT 凭据等（640） |
| Video env | `/etc/hangzhouwan/video.env` | RTSP/RTMP URL 等（640） |

> 三个文件均不提交 Git，含真实凭据。

## 当前生产配置要点

- **推流地址**：正式地址（无 `_bm1684` 后缀），`forbidden_formal_outputs: []`
- **AIS 订阅**：`upAIS/#`（通配符，接收所有基站）
- **B 路 RTSP**：`Channels/301`（偶尔 400 Bad Request，摄像头端固件问题）
- **坐标预测**：sklearn 模式，`coordinate_valid: true`

## 服务名称

- `hangzhouwan.target`
- `hangzhouwan-business.service`
- `hangzhouwan-video.service`

## Socket 位置

- `/run/hangzhouwan/business.sock`（Business Sidecar）
- `/run/hangzhouwan/video-health.sock`（Video 结构化健康接口）

## 热修补丁清单（已应用，需纳入下次 Release 构建）

1. `services/business_enrichment/ais/decoder.py` — JSON 格式 AIS 解析
2. `services/business_enrichment/coordinate/sklearn_predictor.py` — sklearn 警告消除
3. `include/monitoring/pipeline_metrics.h` — e2e_p95/queue_length public 访问器
4. `src/application/dual_stream_application.cpp` — 健康状态填充 e2e_p95/queue_length
5. `dual_stream_app` 二进制 — 已重新编译部署到 release

> ⚠️ Release `manifest.json` SHA256 已失效（热修补丁未重新打包）。下次 `build_release.sh` 时需确保补丁包含在内。

## 待完成项

1. **L1-L4 长测**：参照 `docs/production/manual-long-run-guide.md`
2. **真实 AIS 人工样本**：A/B 各 ≥20 个匹配样本（需有船经过时采集）
3. **Windows 回切演练**
4. **systemd enable**（L4 + 回切演练通过后）
5. **重新构建 Release**：纳入热修补丁，恢复 manifest SHA256

## 已知限制

- **B 路 400 Bad Request**：`Channels/301` 摄像头端固件问题，重启 Video 可恢复；替代通道 `101/201/401/501/701/801` 可用
- **AIS 匹配需有船经过**：匹配半径 500m，当前视野内无 AIS 船舶时 `ais_matched: false` 属正常
- **kern.log 刷屏**：VPU clock 日志，已通过 journald 限制 + rsyslog 轮转控制
- **根分区 5.8G**：已优化至 71%，需定期检查 `kern.log` 膨胀

## 禁止事项

- ❌ 不自动 `systemctl enable`（需 L4 + 回切演练通过）
- ❌ BM1684 和 Windows 不得同时推流到同一正式地址
- ❌ 不提交 `/etc/hangzhouwan/` 真实配置和凭据
- ❌ 不自动执行超过 300 秒的测试
