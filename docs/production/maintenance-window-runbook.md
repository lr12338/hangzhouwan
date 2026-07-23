# 杭州湾 BM1684 VPU 重连修复 · 维护窗口 Runbook

> 候选 Release：`/opt/hangzhouwan/releases/vpu-reconnect-fix-1a1a7b2`
> 修复提交：`1a1a7b2`（分支 `feat/bm1684-edge-deployment`）
> 状态：**维护窗口验证候选，不可上线。维护窗口仍待人工批准。**
> 约束：未获维护窗口批准前不得停止/重启/切换生产；不升级 libsophon/Sophon-FFmpeg；不直接释放 Codec 内部设备地址。
> 本 Runbook 仅给出待执行命令与判定标准，不在批准前实际执行。

仓库根：`/home/linaro/hangzhouwan`　健康工具：`/opt/hangzhouwan/current/bin/hzwctl`　VPU 监控：`bm-smi`

## 0. 定量门禁（全部满足方可签收上线）

| # | 门禁 | 判定方法 | 通过标准 |
|---|---|---|---|
| G1 | RTSP 重连成功率 100% | Step 4 汇总 `overall_rc=0`、phases decoder/rtsp | ok=100 fail=0 |
| G2 | 最终 inflight=0 | 汇总 `phases[].inflight_end` | 各路末轮 inflight=0 |
| G3 | VPU 资源无单调增长 | VPU_HEAP start->final（**所有堆** heap0..heapN） | 各堆 used 持平或下降，无 +39.5MB/轮趋势 |
| G4 | 无非法释放/内存错误 | 原始日志 grep | 无 `invalid free` / `ENOMEM` / `gmem` |
| G5 | RTMP 重连不重建编码器 | Step 6 RTMP e2e 日志 | 编码器创建次数=流路数(双路=2)，100 次重连后不增加 |
| G6 | 退出码 70 触发 systemd 恢复 | systemd 配置审计（已完成） | `Restart=on-failure`；70 非 SuccessExitStatus => 重启；StartLimitBurst=5 防风暴 |
| G7 | 双路输出/推理/推流恢复 | Step 6 健康验收 | `status=HEALTHY`，rtsp/rtmp_connected=true，检测框正常，RTMP 可拉流 |

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
```

- 预期结果：`current` -> `202607221953-3dfcf4e`（旧二进制）；heap2 used≈1937–2031MB（接近耗尽）；服务 active。
- 失败处理：若快照命令失败，记录原因后继续（快照仅用于事后比对，非阻断）。
- 是否继续：是（只读）。

---

## 2. 停止生产（仅维护窗口批准后执行）

```bash
sudo systemctl stop hangzhouwan.target
```

- 预期结果：`hangzhouwan.target`/`-video`/`-business` 均 `inactive`；`pgrep -x dual_stream_app` 无输出。
- 失败处理：若 target 停止但 video 残留，检查 `PartOf=`；`pgrep -x dual_stream_app` 仍有进程时，**不得**强制 kill（除非二次批准），先排查原因。未完全停止不得进入下一步。
- 是否继续：确认 `systemctl is-active hangzhouwan.target` 返回 `inactive` 且无 `dual_stream_app` 进程后继续。

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

- 命令说明：运行器在生产运行时拒绝执行（rc=2）；仅用候选 Release 的 `bin/forced_reconnect_tool`；每阶段超时；任一阶段失败 fail-fast；输出
  `artifacts/internal-development/forced_reconnect_summary.json`（机器可读）+ 各阶段 `.raw`/`.log`。
- 测试矩阵：heap（所有堆基线）/ repro 3 / decoder 100 / rtsp 100 / sweep 5·8·12·20 各 20 / dual 100 / rtmp 决策自检。
- 预期结果：`exit=0`，汇总 `overall_rc=0`、`overall=PASS`；G1（ok=100 fail=0）、G2（inflight=0）、G3（各堆持平/下降）、G4（日志无错误）满足。
- 失败处理：任一阶段 rc!=0 即停止；查看对应 `forced_reconnect_<phase>.raw` 与 summary。常见：熔断（heap avail<60MB，说明 Step 3 未恢复或设备异常）、重连 fail>0（修复回归）。**未通过 G1–G4 不得激活。**
- 是否继续：G1–G4 全部通过后继续。

门禁核验命令（解析汇总）：

```bash
python3 - <<'PY'
import json
d=json.load(open('/home/linaro/hangzhouwan/artifacts/internal-development/forced_reconnect_summary.json'))
print('overall', d['overall'], 'rc', d['overall_rc'])
for p in d['phases']:
    print(p['name'],'rc',p['rc'],'ok',p['ok'],'fail',p['fail'],'inflight',p['inflight_end'],'delta',p['heap2_delta_mb'])
# G4: 扫描原始日志
import glob,subprocess
for f in glob.glob('/home/linaro/hangzhouwan/artifacts/internal-development/forced_reconnect_*.raw'):
    r=subprocess.run(['grep','-ciE','invalid free|ENOMEM|gmem',f],capture_output=True,text=True)
    if int(r.stdout or 0): print('G4 FAIL',f,r.stdout.strip())
PY
```

---

## 5. 激活候选 Release

```bash
bash /home/linaro/hangzhouwan/tools/release/activate_release.sh \
  /opt/hangzhouwan/releases/vpu-reconnect-fix-1a1a7b2
echo "exit=$?"
readlink -f /opt/hangzhouwan/current /opt/hangzhouwan/previous
```

- 预期结果：`exit=0`；`current` -> `vpu-reconnect-fix-1a1a7b2`；`previous` -> `202607221953-3dfcf4e`；
  预检+离线冒烟+原子 `mv -T` 切换+重启+60s smoke 全通过。
- 失败处理：`activate_release.sh` smoke 失败会**自动回滚** previous 并重启、验证 readiness（结果码非 0）。
  自动回滚后**禁止未经状态检查再次手动交换 Release**（见 Step 7）。需排查失败原因后方可重试。
- 是否继续：`exit=0` 且 `current` 指向候选、`previous` 指向旧版后继续。

---

## 6. 健康验收（含 RTMP 端到端 100x 编码器创建计数）

### 6.1 基本健康
```bash
systemctl is-active hangzhouwan.target hangzhouwan-video.service hangzhouwan-business.service
/opt/hangzhouwan/current/bin/hzwctl wait-business --timeout 30
/opt/hangzhouwan/current/bin/hzwctl wait-video --timeout 60
/opt/hangzhouwan/current/bin/hzwctl health
bm-smi
```
- 预期：服务 active；`hzwctl health` JSON `status` 非 `FAILED`（HEALTHY/DEGRADED 中 HEALTHY 为目标），
  `rtsp_connected`/`rtmp_connected` 为 true；heap2 稳定在 ~300MB 量级（无双路泄漏累积）。
- 失败处理：服务未就绪或 status=FAILED，先 `journalctl -u hangzhouwan-video.service` 排查；不可恢复则回滚（Step 7）。
- 是否继续：基本健康通过后进行 6.2。

### 6.2 RTMP 端到端 100x 重连 + 编码器创建计数（G5）
> 目的：证明普通 RTMP 网络重连仅重建 muxer/AVIO，**不重建硬件编码器**。
> 编码器创建日志：`open()` 打印 `信息 | 视频编码 | 编码器：...`；`reconnect_rtmp()` 仅打印 `信息 | RTMP重连 | ...编码器未重建`，不打印编码器创建行。

记下激活后基线编码器创建次数（双路=2）：
```bash
BASE_ENC=$(journalctl -u hangzhouwan-video.service --since "5 min ago" \
  | grep -c '信息 | 视频编码 | 编码器')
echo "baseline encoder_open_count=$BASE_ENC"   # 预期 2（双路各 1）
```

对 RTMP 出口做 100 次快速故障注入（断开/恢复 RTMP 连接），让应用走 `reconnect_rtmp` muxer-only 路径。
> 故障注入方式（任选其一，按现场网络拓扑）：
> - 本地接收端：起本地 RTMP 接收，反复 kill/重启接收进程；
> - 网络：`sudo iptables -A OUTPUT -p tcp --dport 1935 -j DROP` / `sudo iptables -D OUTPUT ...` 循环 100 次；
> - 远端：临时停/启对端 RTMP 服务。
> 以下为网络端口方式示例（每轮断 2s、通 2s）：

```bash
for i in $(seq 1 100); do
  sudo iptables -A OUTPUT -p tcp --dport 1935 -j DROP 2>/dev/null
  sleep 2
  sudo iptables -D OUTPUT -p tcp --dport 1935 -j DROP 2>/dev/null
  sleep 2
  echo "fault round $i done"
done
# 健康接口看重连计数
/opt/hangzhouwan/current/bin/hzwctl health
```

统计编码器创建次数与重连次数：
```bash
ENC_NOW=$(journalctl -u hangzhouwan-video.service --since "20 min ago" \
  | grep -c '信息 | 视频编码 | 编码器')
RTMP_RECONNECT=$(journalctl -u hangzhouwan-video.service --since "20 min ago" \
  | grep -c '信息 | RTMP重连 | 重连成功')
echo "encoder_open_count=$ENC_NOW (baseline=$BASE_ENC)  rtmp_reconnect_success=$RTMP_RECONNECT"
```
- 预期（G5）：`encoder_open_count` == baseline（==2，双路），**未随 100 次重连增加**；
  `rtmp_reconnect_success`≈100；日志无 `DEVICE_RESOURCE_FATAL`/`ENOMEM`。
- 失败处理：编码器创建次数增加（>2）说明普通重连误重建编码器（修复回归）-> 回滚（Step 7）。
- 是否继续：G5 通过后进行 6.3。

### 6.3 双路输出/推理/推流恢复（G7）
```bash
# RTMP 可拉流（任一路，按正式地址）
timeout 15 ffprobe -v error -show_entries format=duration \
  rtmp://hangzhouwanpush.hifleet.com:1935/HangZhouBridge/HangZhouBridgeNorth8_1 || echo "拉流超时/失败"
# 检测推理在产（journal 近期有检测/快照输出）
journalctl -u hangzhouwan-video.service --since "3 min ago" | grep -ciE '检测|snapshot|检测框' || true
# 综合健康
/opt/hangzhouwan/current/bin/hzwctl health
bm-smi
```
- 预期（G7）：RTMP 可拉流；近期有检测输出；`status=HEALTHY`；heap 稳定无单调增长。
- 失败处理：任一不满足 -> 回滚（Step 7）。
- 是否继续：G1–G7 全部满足 -> **签收上线**；否则回滚。

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
> `activate_release.sh` smoke 失败会自动回滚 previous。**自动回滚后禁止未经状态检查再次手动交换 Release**，
> 否则可能在 VPU 堆未恢复时二次击穿。必须依次确认：
```bash
systemctl status hangzhouwan.target --no-pager | head -5      # 非 failed
pgrep -x dual_stream_app && echo OK || echo "ERR: 未运行"      # 旧版应已运行
bm-smi                                                         # heap 回落基线
/opt/hangzhouwan/current/bin/hzwctl health                     # status 非 FAILED
readlink -f /opt/hangzhouwan/current /opt/hangzhouwan/previous # 指向正确
```
- 全部正常后，方可再次排查原因并重试 Step 5；任一异常先恢复状态，不得再次激活。

---

## 附：退出码与 systemd 恢复链（G6 依据）

资源致命 -> `set_resource_fatal()` -> 进程退出码 **70** -> `Restart=on-failure`（70 非零且未列入
`SuccessExitStatus`/`RestartPreventExitStatus`）=> systemd 重启 -> 进程死亡 => 内核回收 VPU 堆 =>
重启后堆恢复基线。`StartLimitBurst=5` + `StartLimitIntervalSec=120s` 限制 120s 内最多 5 次重启，
超限进入 `failed`（需人工 `systemctl reset-failed`），不会形成高频重启风暴。`RestartSec=5` 提供重启间隔。
本轮不执行 `systemctl` 修改或重启（仅审计，已由 `tests/unit/test_systemd_config.py::RestartStormPreventionTest` 覆盖）。
