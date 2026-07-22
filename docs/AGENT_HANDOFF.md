# Agent 接手提示（AGENT_HANDOFF）

## 当前状态：2026-07-22 阶段7板端实装验证完成

项目根目录：`/home/linaro/hangzhouwan-orign/hangzhouwan`
仓库：`https://github.com/lr12338/hangzhouwan`
分支：`feat/bm1684-edge-deployment`
当前 Release：`202607221953-3dfcf4e`（`/opt/hangzhouwan/current`）
平台：BM1684-SOC（chipid `0x1684`，非 BM1684X），libsophon 0.4.9，sophon-ffmpeg 0.8.0

---

## 已完成

- **阶段7板端编译**：C++ 全量编译通过（修复 `video_health_server.cpp` 缺少 `<sys/stat.h>`）
- **systemd 实装**：Business/Video 单元已安装到 `/etc/systemd/system/`（未 enable），`systemd-analyze verify` 通过
- **Wants 替代 Requires**：Video 用 `Wants=hangzhouwan-business.service`，停止 Business 不连带停止 Video
- **tmpfiles.d**：`/run/hangzhouwan` 由 tmpfiles.d 统一管理，重启任一服务不删除另一方的 Socket
- **T1-T10 短测全部通过**：详见 `docs/production/stage7-validation-guide.md`
- **Release 激活/回滚**：原子切换 + 自动回滚 + 60s smoke 验证通过
- **RUNPATH**：二进制嵌入 sophon 库路径，无需 LD_LIBRARY_PATH
- **request_timeout_ms**：从硬编码 30ms 改为配置驱动（默认 200ms），修复 Sidecar 超时
- **Python 测试 93 项通过**，CTest 13/13 通过

## 配置位置

| 文件 | 路径 | 说明 |
|---|---|---|
| 配置 | `/etc/hangzhouwan/application.yaml` | 唯一权威配置（640 root:linaro） |
| Business env | `/etc/hangzhouwan/business.env` | MQTT 凭据等（640） |
| Video env | `/etc/hangzhouwan/video.env` | RTSP/RTMP URL 等（640） |

> 三个文件均不提交 Git，含真实凭据。

## 服务名称

- `hangzhouwan.target`
- `hangzhouwan-business.service`
- `hangzhouwan-video.service`

## Socket 位置

- `/run/hangzhouwan/business.sock`（Business Sidecar）
- `/run/hangzhouwan/video-health.sock`（Video 结构化健康接口）

## 当前生产门禁

- T1-T10 短测全部通过
- 300 秒灰度全链路通过（A 路 10fps/5fps 稳定，0 重连，无泄漏）
- **未通过**：L1-L4 长测、真实 AIS 人工样本、Windows 回切演练、systemd enable

## 已知风险

- **B 路摄像头返回 400 Bad Request**：非代码问题，摄像头端问题，B 路无法连接
- **AIS 缓存为 0**：测试期间无船经过，需有船时采集真实 AIS 样本
- **e2e P95 在健康接口中报告为 0**：journal 中有实际值（~356ms），健康接口采样逻辑待优化
- **TPU info 在健康接口中为 unknown**：bm-smi 集成待完善
- **磁盘 /opt 仅 1.7G 可用**：需监控 Release 累积

## B 路 RTSP 状态

- B 路 RTSP URL：`rtsp://admin:***@112.16.184.176:48554/streaming/Channels/301`
- 状态：摄像头返回 400 Bad Request，无法连接
- 原因：摄像头端配置或固件问题，非 BM1684 代码问题

## 下一步人工测试

1. **L1 30分钟长测**：`docs/production/manual-long-run-guide.md`
2. **L2 2小时长测**
3. **L3 8小时长测**
4. **L4 24小时长测**（通过后才可 enable）
5. **真实 AIS A/B 各 20 个人工样本**：需有船经过时采集
6. **Windows 回切演练**：停止 systemd 服务后确认 Windows 可正常接管
7. **systemd enable**：L4 + 回切演练通过后
8. **正式 RTMP 切换**：灰度对比 24h 后逐路切换

## 禁止事项

- ❌ 不自动 `systemctl enable`（需 L4 + 回切演练通过）
- ❌ 不关闭 Windows 旧服务
- ❌ 不覆盖 Windows 正式 RTMP 地址（灰度使用 `_bm1684` 后缀 Key）
- ❌ 不自动执行超过 300 秒的测试
- ❌ 不提交 `/etc/hangzhouwan/` 真实配置和凭据
- ❌ 不提交测试日志、诊断包、JSONL、输出视频
