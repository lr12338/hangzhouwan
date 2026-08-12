# 生产进度跟踪（PROGRESS）

> 本文件是**当前生产状态的唯一权威索引**。开发阶段历史见 [`history/`](history/)。
> 新开发者先读 [README.md](../README.md)，再读本文件，再按 [00-index.md](00-index.md) 深入。

| 项 | 值 |
|---|---|
| 最后更新 | 2026-08-12（UDS 目录权限事故恢复 + bridge-capture 不可变 Release 激活） |
| 仓库 | `https://github.com/lr12338/hangzhouwan.git` |
| 分支 | `feat/bridge-capture` |
| 当前 HEAD | `21c72fc`（fix(release): package bridge capture service into release） |
| 平台 | BM1684-SOC（chipid `0x1684`），libsophon 0.4.9，sophon-ffmpeg 0.8.0 |
| 生产 Release | `bridge-capture-v1-21c72fc`（`/opt/hangzhouwan/current`） |
| previous | `availability-recovery-v4-20260731-a066254` |
| 系统状态 | 🟡 生产运行中（A 路 HEALTHY，B 路 DEGRADED -- 摄像机 301 通道取流不稳） |
| Bridge 服务 | 🟢 `hangzhouwan-bridge.service` HEALTHY（MQTT connected，records 持续增长） |
| Capture 服务 | 🟡 `hangzhouwan-bridge-capture.service` IDLE（bmodel 加载，UDS 监听，AIS auto trigger 未开启） |
| CTest | `cd build && ctest` -> **21/21 passed**（含 capture_core / capture_protocol / capture_uploader / jpeg_encode） |
| hangzhouwan pytest | `python3 tests/run_tests.py` -> **200/200 passed** |
| bridge pytest | `cd /data/hangzhouwan-bridge && pytest tests/` -> **68/68 passed** |
| 开机自启 | `hangzhouwan.target` enabled（断电自启）；bridge-capture 单独 enabled |
| journald | SystemMaxUse=200M + MaxRetentionSec=7day |

---

## 生产部署历史

| 日期 | Release | 提交 | 内容 |
|---|---|---|---|
| 2026-08-12 | `bridge-capture-v1-21c72fc` | `21c72fc` | 不可变 Release：bridge_capture_app 进 bin/、bridge-capture.env.example 进 config/、bmodel 路径修正、build_release & verify_release 升级；当前生产版本 |
| 2026-07-31 | `availability-recovery-v4-20260731-a066254` | `a066254` | availability-recovery 系列最新；现场恢复部署完成；当前 previous |
| 2026-07-30 | `video-frame-fix-20260730-9c6cec3` | `9c6cec3`（修复 `ce0724b`） | 修复 720p 缩放帧内存契约；A/B 正式 RTMP 软件解码验证通过 |
| 2026-07-28 | `industrial-20260728-fdb878c` | `fdb878c` | 单机工业加固、服务 PID 验证 |
| 2026-07-24 | `log-throttle-20260724-c008eac` | `c008eac` | RTMP 重连日志节流 |
| 2026-07-24 | `vpu-rtmp-pts-fix-20260724-9d449ab` | `9d449ab` | 修复 RTMP muxer-only 重连 PTS 重置致推流自循环 |
| 2026-07-24 | `vpu-reconnect-health-20260724-1eba419` | `1eba419` | 健康判定 v2 |
| 2026-07-22 | `202607221953-3dfcf4e` | `3dfcf4e` | 初始生产部署（已含热修补丁，已被取代） |

完整审计证据见 [`production/maintenance-window-audit-evidence.md`](production/maintenance-window-audit-evidence.md)。

---

## 关键修复记录

### 2026-08-12 UDS 目录权限事故（本次新加）
- **现象**：现场升级到 `bridge-capture-v1-21c72fc` 时 `hzwctl preflight` 失败：
  `❌ UDS 目录可写 / ❌ 目录 /run/hangzhouwan`。Video service 退出 78
  (CONFIG) 后进入 backoff，A 路处理被视频管线拉着跑，B 路失败预存。
- **根因**：`/etc/systemd/system/hangzhouwan-bridge.service` 包含
  `RuntimeDirectory=hangzhouwan` + `RuntimeDirectoryMode=0750`，每次
  bridge 重启都会把 `/run/hangzhouwan` 重建为
  `hangzhouwan-business:hangzhouwan 0750`，覆盖 `/etc/tmpfiles.d/hangzhouwan.conf`
  定义的 `root:hangzhouwan 0770`。group 在 0750 下只有 r-x，video 用户无法
  写 `video-health.sock`。
- **修复**：
  1. 现场：`chown root:hangzhouwan /run/hangzhouwan && chmod 0770 /run/hangzhouwan`
     后 `systemctl restart hangzhouwan-video` 恢复。
  2. 根因：从 `hangzhouwan-bridge.service` 移除 `RuntimeDirectory=hangzhouwan`
     和 `RuntimeDirectoryMode=0750`，由 tmpfiles.d 单一所有者管理
     `/run/hangzhouwan`。
  3. 沉淀：`/data/hangzhouwan/incidents/incident-20260812-1712-snapshot.md`
     记录 INCIDENT_SNAPSHOT。
- **验证**：bridge 重启后 `/run/hangzhouwan` 保持 `root:hangzhouwan 0770`，
  `bridge.sock / business.sock / video-health.sock` 三路并存。

### bridge_capture_app 进不可变 Release（本次新加）
- **问题**：旧 `build_release.sh` 只打包 `dual_stream_app` 和
  `forced_reconnect_tool`，`bridge_capture_app` 编译后留在 build 树
  不进 Release；`hangzhouwan-bridge-capture.service` 单元里
  `--bmodel /opt/hangzhouwan/current/weights/yolov7.bmodel` 路径
  在所有历史 Release 中都不存在（实际 bmodel 在 `models/yolov7_ship_1684_f32.bmodel`）。
- **修复**：
  1. `build_release.sh`：把 `build/bridge_capture_app` 拷到 `$RELEASE_DIR/bin/`，
     把 `config/bridge-capture.env.example` 拷到 `$RELEASE_DIR/config/`。
  2. `verify_release.sh`：在关键文件存在性列表里加 `bin/bridge_capture_app`
     `config/bridge-capture.env.example` `systemd/hangzhouwan-bridge-capture.service`，
     并断言 `bridge_capture_app` 可执行。
  3. `deploy/systemd/hangzhouwan-bridge-capture.service`：bmodel 路径
     修正为 `models/yolov7_ship_1684_f32.bmodel`。
- **验证**：`verify_release.sh` 14/14 通过，PID 核验运行 `dual_stream_app`
  来自新 Release；capture service 启动加载 bmodel、UDS 监听
  `/run/hangzhouwan/bridge-capture.sock` 正常。

### bridge `events_today` 周期统计 KeyError（本次新加）
- **问题**：bridge 服务的 `_stats_loop` 每 30 秒打印
  `stats | events=N today=M ...`，但 `snapshot()` 返回字典里没有
  `events_today` 键，导致每 30 秒一条 `WARNING | stats loop error: 'events_today'`。
- **修复**：`bridge_crossing/stats.py::snapshot()` 在 `events_total` 之后
  新增 `"events_today": self.events_today()`。
- **验证**：bridge 6/6 stats pytest 通过；68/68 bridge pytest 通过；bridge
  重启后日志无 `stats loop error` warning，`bridge-ctl health` 正确显示
  `events_today = 0`。

### 1280×720 推流乱码修复（`ce0724b`，已发布）
- **根因**：BMCV 缩放像素在设备内存，旧代码却向 `h264_bm` 提交未填充的
  主机平面和手工 `data[4..6]`，DMA 输入契约不成立。
- **修复**：缩放后回拷标准主机 YUV420P `AVFrame`，编码器使用
  `is_dma_buffer=0`，并增加尺寸/格式/平面校验和一次性编码前抓帧。
- **验证**：A 路真实 2560×1440 RTSP 到本地 1280×720 硬编、软件解码正常；
  Python 193/193、CTest 17/17（含稳定性脚本）通过。
- **部署验证**：Release 完整性 10/10、预检 46/46、严格健康门禁和 60 秒
  smoke 通过；A/B 编码前帧正常，正式 RTMP 软件解码抓帧及连续 20 秒解码
  均成功且错误为 0。

### RTMP 推流自循环修复（9d449ab）
- **根因**：`1a1a7b2` 将 RTMP 重连改为 muxer-only（保留编码器）但遗留 `pts_.reset()`，
  编码器 DTS 续接而 PTS 归零 -> `pts<dts` 致 FLV 永久写帧失败自循环。
- **修复**：删除 `pts_.reset()`，muxer-only 重连 PTS 续接。

### VPU 显存耗尽修复（1a1a7b2，已含于 1eba419）
- **根因**：RTSP 重连在在途 AVFrame 未释放时关闭 h264_bm 解码器，bm_image 池孤儿化，
  每次重连泄漏 ~39.5MB VPU 显存。
- **修复**：InflightFrameTracker 在途帧计数器 + decoder 关闭顺序保护 + 资源熔断（退出码 70）。

### 假健康修复（4745180，已含于 1eba419）
- VPU 致命/单路断流/重连风暴计入健康状态，hzwctl 醒目展示 status。

---

## 待完成项 / RISKS

| 项 | 状态 | 说明 |
|---|---|---|
| B 路摄像机 301 通道 | 🔶 | 取流灾难性不稳（`Server returned 400 Bad Request`），需排查摄像机端 |
| Capture ROI 校准 | 🟡 | 真实北通航孔摄像头当前 FOV 过宽，YOLO 检测在 47×11 px 级别，< BMCV 32px 最低裁剪门限被拒；需现场标定 `valid_roi` / `capture_roi` 或换更窄 FOV 子流 |
| AIS → Capture 自动 trigger | ⏸️ | `bridge.yaml::capture.enabled` 仍为 false；ROI 校准前不能放开，否则每次小目标 ARM 都会触发 0 字节写入浪费 VPU |
| RTMP 服务器丢连接 | 🔶 | 双路 ~15-18/min 重连，服务器约每 3.5s 丢连接，待排查 |
| G1 真实 RTSP 100 轮 | ⏳ | 环境无真实断流源，风险豁免待补 |
| G5 RTMP 100x 故障注入 | ⏳ | 需维护窗口 |
| 长稳 2-4h | ⏳ | 需维护窗口 |
| OOM 保护 | ⏳ | 无 swap，建议设 OOMScoreAdjust=-500 |

---

## 端到端抓拍验证（G3 / G4 / G5 实际执行）

- **G3 Local real-ship MP4**（`/data/hangzhouwan-bridge/test_video/测试船舶抓拍.mp4`）：
  `bridge_capture_app --cli-arm north --cli-file 测试船舶抓拍.mp4 --conf 0.05 --iou 0.1`
  → `north_414402810_..._cli-1786.jpg` 15988 字节 494×98 落盘。
- **G4 Capture JPEG**：JPEG 头 `JFIF 1.01, 494x98, components 3`。
- **G5 Repeated VPU lifecycle**：连续 5 次会话（每次都是新进程）均 `captured_total=1`、
  `state=IDLE` 干净退出，无 VPU 泄漏告警。
- **G11 Real North camera**（`rtsp://admin:***@112.16.184.176:48554/Channels/801`）：
  UDS `arm` 命令接受，rtsp 连接 2560×1440@25fps，YOLO 检出 bbox (2104,629,2151,640)
  大小 47×11 px，padding 后 60×13 < BMCV 最低 32px → 正确被拒。
  → 当前真实摄像头在当前 ROI/置信度下只能产出过小裁剪，需 ROI 校准或 FOV 调整。

> 历史开发阶段（阶段 1-9）的详细过程报告见 [`history/`](history/)。
