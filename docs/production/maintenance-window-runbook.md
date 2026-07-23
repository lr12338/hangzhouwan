# 杭州湾 BM1684 VPU 重连修复 · 维护窗口 Runbook

> 候选 Release：`/opt/hangzhouwan/releases/vpu-reconnect-fix-1a1a7b2`
> 修复提交：`1a1a7b2`（分支 `feat/bm1684-edge-deployment`）
> 状态：**维护窗口验证候选，不可上线。维护窗口仍待人工批准。**
> 约束：未获维护窗口批准前不得停止/重启/切换生产；不升级 libsophon/Sophon-FFmpeg；不直接释放 Codec 内部设备地址。
> 本 Runbook 仅给出待执行命令与判定标准，不在批准前实际执行。

仓库根：`/home/linaro/hangzhouwan`　健康工具：`/opt/hangzhouwan/current/bin/hzwctl`　VPU 监控：`bm-smi`

## 维护窗口预计时长

| 阶段 | 预计耗时 | 最坏耗时 | 视频是否可用 |
|------|---------|---------|------------|
| Step 0–1 执行前快照 | 3 min | 5 min | ✅ 可用（只读） |
| Step 2 停止生产 | 10 s | 30 s | ❌ 不可用 |
| Step 3 资源恢复确认 | 15 s | 60 s | ❌ 不可用 |
| Step 4 forced-reconnect 测试 | 25 min | 60 min | ❌ 不可用 |
| Step 5 激活候选 Release | 2 min | 4 min | ❌ 不可用（重启中断 ~60s） |
| Step 6.1 基本健康 | 2 min | 3 min | ✅ 恢复可用 |
| Step 6.2 RTMP 故障注入 | 7 min | 10 min | ⚠️ 间歇中断（每轮 2s） |
| Step 6.3 双路验收 | 3 min | 5 min | ✅ 可用 |
| **预计视频不可用总时长** | **~30 min** | | |
| **最坏视频不可用总时长** | | **~70 min** | |
| Step 7 回滚（如需） | +2 min | +4 min | ❌ 追加不可用 |

> ⚠️ 不得继续使用 30–60 秒作为整个维护窗口中断估计。测试阶段（Step 4）占不可用时间的主要部分。
> RTMP 故障注入期间（Step 6.2），每轮 2 秒断网导致 RTMP 输出中断，但视频采集和推理不受影响。

## 0. 定量门禁（全部满足方可签收上线）

| # | 门禁 | 判定方法 | 通过标准 |
|---|---|---|---|
| G0 | 启动门禁 | Step 6.1 + `hzwctl status`/`version` | Business+Video active；双路 RTSP/RTMP connected；双路 output_fps≥5、inference_fps>0；MQTT/AIS 正常；`status=HEALTHY`；Release 与 commit 一致；无 VPU 分配错误；`previous` 指向有效回滚目标 |
| G1 | RTSP 重连成功率 100% | Step 4 汇总 `gate_results` G1 | decoder ok=100 fail=0 **且** 真实 RTSP 断流验证通过（当前环境未实现 → G1 保持 NOT_PASSED） |
| G2 | 最终 inflight=0 | 汇总 `gate_results` G2 | 各路末轮 inflight=0 |
| G3 | VPU 资源无单调增长 | 汇总 `gate_results` G3 + `vpu_heap_analysis` | 所有堆各检查点 delta ≤ +10MB，无单调增长趋势 |
| G4 | 无非法释放/内存错误 | 汇总 `gate_results` G4 | 无 `invalid free` / `ENOMEM` / `gmem` / `BMVidDecSeqInit` 失败 |
| G5 | RTMP 重连不重建编码器 | Step 6.2 `rtmp_fault_injection.sh` 输出 | 编码器创建次数=2（双路），100 次重连后不增加 |
| G6 | 退出码 70 触发 systemd 恢复 | Step 6.4 三级核验 | 静态审计 ✅ + 现场配置核验 ✅ + 真实恢复验证（本轮不执行，G6 端到端未完成） |
| G7 | 双路输出/推理/推流恢复 | Step 6.3 健康验收 | `status=HEALTHY`（DEGRADED 不得签收），见通过标准明细 |

> **G1 阻断说明**：当前环境无真实 RTSP 源断流能力，`decoder_reopen_simulation` 阶段仅模拟文件源解码器换建，
> 不等于真实 RTSP 断流验证。**G1 保持 NOT_PASSED，明确阻止激活。** 禁止用模拟测试冒充真实 RTSP 门禁。
> G1 未通过 → 整体 `overall_rc` 非零 → 汇总 `overall=FAIL` → **不得激活**。

---

## 1. 执行前快照（生产运行中，只读，不停产）

```bash
SNAP_DIR=/home/linaro/hangzhouwan/artifacts/internal-development/maint-window
mkdir -p "$SNAP_DIR"
TS=$(date -u +%Y%m%dT%H%M%SZ)
# VPU 堆现状（所有堆）
bm-smi > "$SNAP_DIR/pre_bm-smi_$TS.txt" 2>&1
# 当前/previous 指向
readlink -f /opt/hangzhouwan/current /opt/hangzhouwan/previous | tee "$SNAP_DIR/pre_releases_$TS.txt"
# 服务状态
systemctl is-active hangzhouwan.target hangzhouwan-video.service hangzhouwan-business.service \
  | tee "$SNAP_DIR/pre_services_$TS.txt"
# 健康基线
/opt/hangzhouwan/current/bin/hzwctl health > "$SNAP_DIR/pre_health_$TS.json" 2>&1 || true
# journal 基线（用于事后比对编码器创建次数）
journalctl -u hangzhouwan-video.service --since "10 min ago" > "$SNAP_DIR/pre_journal_$TS.log" 2>&1
# systemd 实际加载配置（G6 现场核验）
systemctl cat hangzhouwan-video.service > "$SNAP_DIR/pre_systemd_cat_video_$TS.txt" 2>&1
systemctl cat hangzhouwan-business.service > "$SNAP_DIR/pre_systemd_cat_business_$TS.txt" 2>&1
systemctl cat hangzhouwan.target > "$SNAP_DIR/pre_systemd_cat_target_$TS.txt" 2>&1
systemctl show hangzhouwan-video.service -p Restart,RestartUSec,StartLimitBurst,StartLimitIntervalUSec,ExecStart,SuccessExitStatus,RestartPreventExitStatus \
  | tee "$SNAP_DIR/pre_systemd_show_video_$TS.txt"
systemctl show hangzhouwan-business.service -p Restart,RestartUSec,StartLimitBurst,StartLimitIntervalUSec,SuccessExitStatus,RestartPreventExitStatus \
  | tee "$SNAP_DIR/pre_systemd_show_business_$TS.txt"
```

### 1.1 阻断项核验（失败不得继续）

以下项目为**阻断项**，任一失败立即停止，不得进入 Step 2：

```bash
# 阻断项 1: current 指向预期旧版
CUR_REAL=$(readlink -f /opt/hangzhouwan/current)
[ "$CUR_REAL" = "/opt/hangzhouwan/releases/202607221953-3dfcf4e" ] \
  || { echo "❌ 阻断: current 未指向 202607221953-3dfcf4e: $CUR_REAL"; exit 1; }

# 阻断项 2: previous 存在且指向有效目录
PREV_REAL=$(readlink -f /opt/hangzhouwan/previous)
[ -d "$PREV_REAL" ] || { echo "❌ 阻断: previous 无效: $PREV_REAL"; exit 1; }

# 阻断项 3: 候选 Release 完整性校验
bash /home/linaro/hangzhouwan/tools/release/verify_release.sh \
  /opt/hangzhouwan/releases/vpu-reconnect-fix-1a1a7b2 \
  || { echo "❌ 阻断: 候选 Release verify 失败"; exit 1; }

# 阻断项 4: 候选 manifest commit = 1a1a7b2
MANIFEST_COMMIT=$(python3 -c "import json; print(json.load(open('/opt/hangzhouwan/releases/vpu-reconnect-fix-1a1a7b2/manifest.json'))['commit'])")
[ "$MANIFEST_COMMIT" = "1a1a7b2" ] \
  || { echo "❌ 阻断: manifest commit=$MANIFEST_COMMIT != 1a1a7b2"; exit 1; }

# 阻断项 5: systemd 实际配置核验（G6 现场配置核验）
VIDEO_RESTART=$(systemctl show hangzhouwan-video.service -p Restart --value)
[ "$VIDEO_RESTART" = "on-failure" ] || { echo "❌ 阻断: video Restart=$VIDEO_RESTART (应 on-failure)"; exit 1; }
VIDEO_BURST=$(systemctl show hangzhouwan-video.service -p StartLimitBurst --value)
[ "$VIDEO_BURST" -le 5 ] || { echo "❌ 阻断: video StartLimitBurst=$VIDEO_BURST (>5)"; exit 1; }

echo "✅ 全部阻断项通过"
```

- 预期结果：`current` -> `202607221953-3dfcf4e`；heap2 used≈1937–2031MB；服务 active；systemd 配置符合预期。
- 失败处理：**阻断项失败不得继续**，记录原因后上报，等待排查修复后重新执行。
- 是否继续：全部阻断项通过后继续。

---

## 2. 停止生产（仅维护窗口批准后执行）

```bash
sudo systemctl stop hangzhouwan.target
```

- 预期结果：`hangzhouwan.target`/`-video`/`-business` 均 `inactive`；`pgrep -x dual_stream_app` 无输出。
- 失败处理：若 target 停止但 video 残留，检查 `PartOf=`；`pgrep -x dual_stream_app` 仍有进程时，**不得**强制 kill（除非二次批准），先排查原因。未完全停止不得进入下一步。
- 是否继续：确认 `systemctl is-active hangzhouwan.target` 返回 `inactive` 且无 `dual_stream_app` 进程后继续。

### 2.1 关于停止范围的评估

当前方案停止 `hangzhouwan.target`（同时停止 video + business）。评估是否只需停止 video：

- **Business sidecar**（坐标预测 + MQTT AIS）不使用 VPU，可保持运行。
- **Video 服务**使用 VPU，必须停止以释放 VPU 预算。
- Video 的 `ExecStartPre=hzwctl wait-business` 依赖 business 运行，因此重启 video 时 business 需先就绪。
- **结论**：测试阶段（Step 4）仅需停止 video 释放 VPU，可保留 business 运行以减少恢复时间。
  但激活阶段（Step 5）需重启完整 target 确保 business/video 版本一致。
- **本 Runbook 保留停止整个 target 的方案**，理由：激活涉及 Release 切换，business 和 video 二进制均来自同一 Release，需同步重启确保一致性。停止 business 的额外影响仅为 AIS/坐标预测短暂中断（~30 min），不影响摄像头视频采集本身。

---

## 3. 资源恢复确认

```bash
pgrep -x dual_stream_app && echo "ERR: 残留进程" || echo "OK: 无残留"
bm-smi
```

- 预期结果：无残留进程；heap2 used 由 ~2000MB 回落至基线 ~300MB（进程死亡 -> 内核回收 VPU 堆）。
- 失败处理：若 heap 未回落，可能存在其他占用 VPU 的残留进程（`pgrep -af bm_` / `fuser`）；未恢复前**禁止**运行压测（会击穿设备）。排查并恢复后重试本步。
- 是否继续：heap 回落至基线、无残留进程后继续。

---

## 4. 测试（VPU forced-reconnect，生产已停止）

```bash
cd /home/linaro/hangzhouwan
HZW_CANDIDATE_RELEASE=/opt/hangzhouwan/releases/vpu-reconnect-fix-1a1a7b2 \
  bash tools/dual_stream/forced_reconnect_run.sh all
echo "exit=$?"
```

- 命令说明：运行器在生产运行时拒绝执行（rc=2，无 HZW_FORCE_RUN 绕过）；仅用候选 Release 的 `bin/forced_reconnect_tool`；每阶段超时；任一阶段失败 fail-fast；输出唯一 `forced_reconnect_summary_<run_id>.json`（机器可读）+ 各阶段 `.raw`/`.log`。
- 测试矩阵：heap（所有堆基线）/ repro 3 / decoder 100 / decoder_reopen_simulation 100 / sweep 5·8·12·20 各 20 / dual 100 / rtmp 决策自检。
- **`decoder_reopen_simulation` 是文件源解码器换建模拟，非真实 RTSP 断流。** G1 因此保持 NOT_PASSED。
- 预期结果：`exit` 非零（因 G1 NOT_PASSED）；汇总 `overall=FAIL`、`failure_reason=gate_failed:G1`。
  G2/G3/G4 应为 PASS（若 VPU 无泄漏、无内存错误）。
- 失败处理：查看对应 `forced_reconnect_<phase>.raw` 与 summary 的 `gate_results`。常见：熔断（heap avail<60MB）、重连 fail>0（修复回归）。**未通过 G1–G4 不得激活。**
- 是否继续：G2/G3/G4 通过后，G1 需另行安排真实 RTSP 测试。本轮 G1 未通过 → **不得激活**。

门禁核验命令（解析汇总，含 gate_results）：

```bash
python3 - <<'PY'
import json, glob, os
# 找最新的 summary 文件
summaries = sorted(glob.glob('/home/linaro/hangzhouwan/artifacts/internal-development/forced_reconnect_summary_*.json'))
# 排除 latest 副本
summaries = [s for s in summaries if 'latest' not in s]
if not summaries:
    print('未找到 summary 文件'); exit(1)
f = summaries[-1]
print(f'summary: {os.path.basename(f)}')
d = json.load(open(f))
print(f'overall: {d["overall"]} rc={d["overall_rc"]} reason={d.get("failure_reason","")}')
print(f'run_id: {d.get("run_id","")}')
print(f'candidate_manifest_commit: {d.get("candidate_manifest_commit","")}')
print(f'script_commit: {d.get("script_commit","")}')
print()
for g in d.get('gate_results', []):
    print(f'{g["gate"]}: {g["status"]} - {g["detail"]}')
print()
for p in d['phases']:
    print(f'  {p["name"]}: rc={p["rc"]} ok={p["ok"]} fail={p["fail"]} inflight={p["inflight_end"]} delta={p["heap2_delta_mb"]}')
PY
```

---

## 5. 激活候选 Release

> ⚠️ **G1 未通过时不得执行此步骤。** 当前 G1 为 NOT_PASSED，本步骤仅作为将来 G1 通过后的参考。

```bash
bash /home/linaro/hangzhouwan/tools/release/activate_release.sh \
  /opt/hangzhouwan/releases/vpu-reconnect-fix-1a1a7b2
echo "exit=$?"
readlink -f /opt/hangzhouwan/current /opt/hangzhouwan/previous
```

- 预期结果：`exit=0`；`current` -> `vpu-reconnect-fix-1a1a7b2`；`previous` -> `202607221953-3dfcf4e`；
  verify + preflight（hzwctl 缺失硬失败不跳过）+ 离线冒烟 + 原子 `mv -T` 切换 + 重启（失败硬失败+回滚）+ Business/Video 就绪 + 60s smoke + PID 核验全通过。
- 结果码说明：
  - `0` 成功
  - `3` hzwctl 缺失（硬失败，不跳过预检）
  - `50` systemctl restart 失败（进入受控回滚）
  - `8` PID 核验失败（运行的可执行文件不来自候选）
  - `90` 自动回滚成功但 health 未达 HEALTHY
  - `99` 自动回滚失败（严重，需人工介入）
- 失败处理：`activate_release.sh` 任何失败会**自动回滚** previous 并重启、验证 Business+Video readiness + health。
  自动回滚失败输出结果码 99（严重错误），不得仅视为"回滚完成"。
  自动回滚后**禁止未经状态检查再次手动交换 Release**（见 Step 7）。
- 是否继续：`exit=0` 且 `current` 指向候选、`previous` 指向旧版、PID 核验通过后继续。

---

## 6. 健康验收

### 6.1 基本健康
```bash
systemctl is-active hangzhouwan.target hangzhouwan-video.service hangzhouwan-business.service
/opt/hangzhouwan/current/bin/hzwctl wait-business --timeout 30
/opt/hangzhouwan/current/bin/hzwctl wait-video --timeout 60
/opt/hangzhouwan/current/bin/hzwctl health
bm-smi
```
- 预期：服务 active；`hzwctl health` JSON `status` 非 `FAILED`；`rtsp_connected`/`rtmp_connected` 为 true；heap2 稳定在 ~300MB 量级。
- 失败处理：服务未就绪或 status=FAILED，先 `journalctl -u hangzhouwan-video.service` 排查；不可恢复则回滚（Step 7）。
- 是否继续：基本健康通过后进行 6.2。

### 6.2 RTMP 端到端 100x 重连 + 编码器创建计数（G5）

> 使用专用安全脚本 `rtmp_fault_injection.sh`，不再保留"任选其一"的开放式操作。
> 脚本安全保证：`set -euo pipefail`、唯一规则标识、`trap EXIT/INT/TERM` 删除测试规则、
> 仅限目标 IP + 1935 端口、前后核对无残留、journal cursor 精确统计、A/B 分别计数。

```bash
# 必须指定 RTMP 目标 IP（按现场实际地址）
bash /home/linaro/hangzhouwan/tools/dual_stream/rtmp_fault_injection.sh \
  --target-ip <RTMP_SERVER_IP> --rounds 100 --break-s 2 --restore-s 2
echo "exit=$?"
```

- 预期（G5）：
  - 编码器创建次数 = baseline = 2（双路各 1），100 次重连后**不增加**
  - RTMP 重连次数：每路各 100 次（A=100, B=100），总计 200 次
  - 无 `DEVICE_RESOURCE_FATAL` / `ENOMEM` / `invalid free`
  - 无残留 iptables 规则
- 重连次数说明：100 轮断网，每路预期各 100 次重连（总计 200 次），不是总计 100 次。
- 编码器创建计数：只能保持双路初始 2 次，不得增加。
- 失败处理：编码器创建次数增加（>2）说明普通重连误重建编码器（修复回归）-> 回滚（Step 7）。
- 是否继续：G5 通过后进行 6.3。

### 6.3 双路输出/推理/推流恢复（G7）

```bash
# RTMP 可拉流（双路分别验证，按正式地址）
timeout 15 ffprobe -v error -show_entries format=duration \
  rtmp://hangzhouwanpush.hifleet.com:1935/HangZhouBridge/HangZhouBridgeNorth8_1 || echo "A路拉流超时/失败"
timeout 15 ffprobe -v error -show_entries format=duration \
  rtmp://hangzhouwanpush.hifleet.com:1935/HangZhouBridge/HangZhouBridgeSouth8_1 || echo "B路拉流超时/失败"
# 检测推理在产（journal 近期有检测/快照输出）
journalctl -u hangzhouwan-video.service --since "3 min ago" | grep -ciE '检测|snapshot|检测框' || true
# 综合健康
/opt/hangzhouwan/current/bin/hzwctl health
bm-smi
```

**G7 通过标准明细（全部满足方可签收）：**

| 检查项 | 通过标准 |
|--------|---------|
| 双路 RTSP | A 路 `rtsp_connected=true` 且 B 路 `rtsp_connected=true` |
| 双路 RTMP | A 路 `rtmp_connected=true` 且 B 路 `rtmp_connected=true` |
| output_fps | A 路和 B 路 `output_fps > 0`（双路均在产帧） |
| inference_fps | A 路和 B 路 `inference_fps > 0`（双路均在推理） |
| AIS | business sidecar MQTT AIS 订阅活跃（journal 有 AIS 消息） |
| MQTT | `mqtt_connected=true` |
| Business Socket | `business_socket_connected=true`（UDS 连接正常） |
| 检测叠加 | journal 有检测框/快照输出（推理结果在渲染） |
| 整体状态 | `status=HEALTHY` |

- **DEGRADED 策略**：`status=DEGRADED` 只能进入限时排查（最长 10 min），不得签收上线。排查后恢复 HEALTHY 可签收；超时未恢复则回滚。
- **HEALTHY 签收**：仅 `status=HEALTHY` 且上述全部检查项满足方可正式签收上线。
- 失败处理：任一不满足 -> 回滚（Step 7）。
- 是否继续：G1–G7 全部满足 -> **签收上线**；否则回滚。

### 6.4 退出码 70 systemd 恢复核验（G6）

G6 分为三个验证层级，必须明确区分：

**层级 1 — 仓库静态审计（已完成）**
- 检查 `deploy/systemd/` 下的 unit 文件，验证 `Restart=on-failure`、`StartLimitBurst≤5`、`StartLimitIntervalSec≤120s`、无 `SuccessExitStatus`/`RestartPreventExitStatus`。
- 覆盖：`tests/unit/test_systemd_config.py::RestartStormPreventionTest`
- 状态：✅ 已完成

**层级 2 — 现场加载配置核验（执行前快照已采集，需人工比对）**
```bash
# 读取板端实际生效的 systemd 配置
systemctl cat hangzhouwan-video.service
systemctl show hangzhouwan-video.service \
  -p Restart,RestartUSec,StartLimitBurst,StartLimitIntervalUSec,SuccessExitStatus,RestartPreventExitStatus,ExecStart

systemctl cat hangzhouwan-business.service
systemctl show hangzhouwan-business.service \
  -p Restart,RestartUSec,StartLimitBurst,StartLimitIntervalUSec,SuccessExitStatus,RestartPreventExitStatus
```
- 核验项：
  - `Restart=on-failure`（退出码 70 非零，触发重启）
  - `SuccessExitStatus` 为空（70 不被误判为成功）
  - `RestartPreventExitStatus` 为空（70 不被排除重启）
  - `StartLimitBurst≤5`（防风暴）
  - `StartLimitIntervalUSec≤120s`（限流窗口）
  - `RestartUSec≥5s`（重启间隔）
- 状态：执行前快照已采集（Step 1），需人工比对确认与仓库一致。

**层级 3 — 真实恢复验证（本轮不执行，G6 端到端未完成）**
- 方法设计（安全、受控，需单独维护窗口批准）：
  1. 确认生产正常运行且 VPU heap 有余量
  2. 向 `dual_stream_app` 发送 SIGRTMIN+1 或通过测试注入点触发 `set_resource_fatal()`（退出码 70）
  3. 观察 systemd 自动重启（`journalctl -u hangzhouwan-video.service --since "1 min ago"`）
  4. 验证进程重启后 VPU heap 回落基线（`bm-smi`）
  5. 验证 `status` 恢复 HEALTHY
  6. 验证 `StartLimitBurst` 计数未超限
- **本轮不执行此验证。** 没有真实执行退出码 70 恢复测试时，**不得宣称端到端 G6 已验证**。
- G6 当前状态：层级 1 ✅ + 层级 2 ✅ + 层级 3 ❌（未执行）= **G6 端到端未完成**。

---

## 7. 回滚

### 7.1 人工回滚
```bash
bash /home/linaro/hangzhouwan/tools/release/rollback_release.sh
readlink -f /opt/hangzhouwan/current /opt/hangzhouwan/previous
/opt/hangzhouwan/current/bin/hzwctl wait-business --timeout 30
/opt/hangzhouwan/current/bin/hzwctl wait-video --timeout 60
/opt/hangzhouwan/current/bin/hzwctl health
bm-smi
```
- 预期：`current` <-> `previous` 交换并切回旧版；重启后双路恢复 HEALTHY；heap 回落基线。
- 说明：仅软链接切换 + 重启 + readiness，不重新编译、不 `git checkout`。

### 7.2 自动回滚后的强制状态检查（禁止盲目再次交换）
> `activate_release.sh` 任何失败会自动回滚 previous，验证 Business+Video readiness + health。
> 自动回滚失败输出结果码 99（严重错误），需人工介入。
> **自动回滚后禁止未经状态检查再次手动交换 Release**，否则可能在 VPU 堆未恢复时二次击穿。必须依次确认：
```bash
systemctl status hangzhouwan.target --no-pager | head -5      # 非 failed
pgrep -x dual_stream_app && echo OK || echo "ERR: 未运行"      # 旧版应已运行
bm-smi                                                         # heap 回落基线
/opt/hangzhouwan/current/bin/hzwctl health                     # status 非 FAILED
readlink -f /opt/hangzhouwan/current /opt/hangzhouwan/previous # 指向正确
```
- 全部正常后，方可再次排查原因并重试 Step 5；任一异常先恢复状态，不得再次激活。
- 若自动回滚结果码为 99（回滚失败），**立即停止操作**，人工排查 systemd 状态和 VPU heap，不得尝试任何自动恢复。

---

## 附：退出码与 systemd 恢复链（G6 依据）

资源致命 -> `set_resource_fatal()` -> 进程退出码 **70** -> `Restart=on-failure`（70 非零且未列入
`SuccessExitStatus`/`RestartPreventExitStatus`）=> systemd 重启 -> 进程死亡 => 内核回收 VPU 堆 =>
重启后堆恢复基线。`StartLimitBurst=5` + `StartLimitIntervalSec=120s` 限制 120s 内最多 5 次重启，
超限进入 `failed`（需人工 `systemctl reset-failed`），不会形成高频重启风暴。`RestartSec=5` 提供重启间隔。
本轮不执行 `systemctl` 修改或重启（仅审计，已由 `tests/unit/test_systemd_config.py::RestartStormPreventionTest` 覆盖静态审计）。
