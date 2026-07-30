# 生产进度跟踪（PROGRESS）

> 本文件是**当前生产状态的唯一权威索引**。开发阶段历史见 [`history/`](history/)。
> 新开发者先读 [README.md](../README.md)，再读本文件，再按 [00-index.md](00-index.md) 深入。

| 项 | 值 |
|---|---|
| 最后更新 | 2026-07-30（1280×720 推流画面修复生产激活验证） |
| 仓库 | `https://github.com/lr12338/hangzhouwan.git` |
| 分支 | `feat/bm1684-edge-deployment` |
| 画面修复提交 | `ce0724b` |
| 平台 | BM1684-SOC（chipid `0x1684`），libsophon 0.4.9，sophon-ffmpeg 0.8.0 |
| 生产 Release | `video-frame-fix-20260730-9c6cec3`（`/opt/hangzhouwan/current`） |
| previous | `industrial-20260728-fdb878c` |
| 系统状态 | 🟡 生产运行中（A 路 HEALTHY，B 路 DEGRADED -- 摄像机 301 通道取流不稳） |
| Python 测试 | `python3 -m pytest -q` -> **193 passed** |
| CTest | `cd build && ctest` -> **17/17 passed**（含稳定性脚本） |
| 开机自启 | `hangzhouwan.target` enabled（断电自启） |
| journald | SystemMaxUse=200M + MaxRetentionSec=7day |

---

## 生产部署历史

| 日期 | Release | 提交 | 内容 |
|---|---|---|---|
| 2026-07-30 | `video-frame-fix-20260730-9c6cec3` | `9c6cec3`（修复 `ce0724b`） | 修复 720p 缩放帧内存契约；A/B 正式 RTMP 软件解码验证通过；当前生产版本 |
| 2026-07-28 | `industrial-20260728-fdb878c` | `fdb878c` | 单机工业加固、服务 PID 验证；当前回滚版本 |
| 2026-07-24 | `log-throttle-20260724-c008eac` | `c008eac` | RTMP 重连日志节流（每10次汇总1条，-80%日志量） |
| 2026-07-24 | `vpu-rtmp-pts-fix-20260724-9d449ab` | `9d449ab` | 修复 RTMP muxer-only 重连 PTS 重置致推流自循环 |
| 2026-07-24 | `vpu-reconnect-health-20260724-1eba419` | `1eba419` | 健康判定 v2（重连宽限+防抖+阈值集中配置） |
| 2026-07-22 | `202607221953-3dfcf4e` | `3dfcf4e` | 初始生产部署（已含热修补丁，已被取代） |

完整审计证据见 [`production/maintenance-window-audit-evidence.md`](production/maintenance-window-audit-evidence.md)。

---

## 关键修复记录

### 1280×720 推流乱码修复（`ce0724b`，已发布）
- **根因**：BMCV 缩放像素在设备内存，旧代码却向 `h264_bm` 提交未填充的
  主机平面和手工 `data[4..6]`，DMA 输入契约不成立。
- **修复**：缩放后回拷标准主机 YUV420P `AVFrame`，编码器使用
  `is_dma_buffer=0`，并增加尺寸/格式/平面校验和一次性编码前抓帧。
- **验证**：A 路真实 2560×1440 RTSP 到本地 1280×720 硬编、软件解码正常；
  Python 193/193、CTest 17/17（含稳定性脚本）通过。
- **部署验证**：Release 完整性 10/10、预检 46/46、严格健康门禁和 60 秒
  smoke 通过；A/B 编码前帧正常，正式 RTMP 软件解码抓帧及连续 20 秒解码
  均成功且错误为 0。诊断环境已清除，Video 再次重启后复验成功。

### RTMP 推流自循环修复（9d449ab）
- **根因**：`1a1a7b2` 将 RTMP 重连改为 muxer-only（保留编码器）但遗留 `pts_.reset()`，
  编码器 DTS 续接而 PTS 归零 -> `pts<dts` 致 FLV 永久写帧失败自循环。
- **修复**：删除 `pts_.reset()`，muxer-only 重连 PTS 续接。
- **验证**：`pts<dts` 错误从数百/分钟降至 0，B 路 output_fps 从 0.5 恢复至 2-4。

### VPU 显存耗尽修复（1a1a7b2，已含于 1eba419）
- **根因**：RTSP 重连在在途 AVFrame 未释放时关闭 h264_bm 解码器，bm_image 池孤儿化，
  每次重连泄漏 ~39.5MB VPU 显存。
- **修复**：InflightFrameTracker 在途帧计数器 + decoder 关闭顺序保护 + 资源熔断（退出码 70）。

### 假健康修复（4745180，已含于 1eba419）
- VPU 致命/单路断流/重连风暴计入健康状态，hzwctl 醒目展示 status。

---

## 待完成项

| 项 | 状态 | 说明 |
|---|---|---|
| 720p 乱码修复长稳观察 | 🔶 | G8 画面验证通过；继续观察 2-4h，建议 24h |
| B 路摄像机 301 通道 | 🔶 | 取流灾难性不稳（`最近输入` 最大 80s），需排查摄像机端 |
| RTMP 服务器丢连接 | 🔶 | 双路 ~15-18/min 重连，服务器约每 3.5s 丢连接，待排查 |
| G1 真实 RTSP 100 轮 | ⏳ | 环境无真实断流源，风险豁免待补 |
| G5 RTMP 100x 故障注入 | ⏳ | 需维护窗口 |
| 长稳 2-4h | ⏳ | 需维护窗口 |
| OOM 保护 | ⏳ | 无 swap，建议设 OOMScoreAdjust=-500 |

> 历史开发阶段（阶段 1-9）的详细过程报告见 [`history/`](history/)。
