# Windows 服务替换方案

> 目标：将 Windows 旧服务替换为 BM1684 双路检测系统。
> **当前状态：BM1684 已替代 Windows 旧服务，双路推流正式地址，生产运行中。**

## 当前状态

BM1684 双路检测系统已完成以下替换步骤：

- ✅ BM1684 推流地址已切换为正式 RTMP 地址（`HangZhouBridgeNorth8_1` / `HangZhouBridgeNorth3_1`）
- ✅ `forbidden_formal_outputs` 已清空，preflight 不再拦截正式地址
- ✅ 双路 RTSP 取流正常（A路 Channels/801，B路 Channels/301）
- ✅ 双路 RTMP 推流正常（`RTMP失败=0`，`RTSP重连=0`）
- ✅ 坐标预测正常（COORD_ONLY，`coordinate_valid: true`）
- ✅ AIS 数据接收正常（`upAIS/#` 通配符订阅，缓存 300+ 艘船）
- ✅ 系统状态 HEALTHY，双路 output_fps ~10

### 待完成项

- [ ] 24 小时长测（L4）通过
- [ ] Windows 回切演练
- [ ] `sudo systemctl enable hangzhouwan.target`（开机自启）

---

## 替换原则

1. BM1684 直接推流到正式 RTMP 地址
2. Windows 旧服务停止推流（避免双推冲突）
3. BM1684 稳定运行 24 小时后 enable 开机自启
4. Windows 保留 3-7 天作为回切兜底
5. 失败立即回切

---

## 已完成步骤

### 阶段 1：灰度对比（已完成）

- BM1684 曾使用 `_bm1684` 后缀灰度地址推流
- 灰度地址 `HangZhouBridgeNorth8_1_bm1684` 在 get 服务器可正常拉流验证
- 灰度流确认正常后切换为正式地址

### 阶段 2：切换正式地址（已完成）

1. 修改 `/etc/hangzhouwan/video.env`：
   - `STREAM_A_OUTPUT_URL=rtmp://hangzhouwanpush.hifleet.com:1935/HangZhouBridge/HangZhouBridgeNorth8_1`
   - `STREAM_B_OUTPUT_URL=rtmp://hangzhouwanpush.hifleet.com:1935/HangZhouBridge/HangZhouBridgeNorth3_1`
2. 修改 `/etc/hangzhouwan/application.yaml`：
   - `deploy.forbidden_formal_outputs: []`
3. 重启 Business + Video 服务
4. 验证正式地址可拉流：
   ```bash
   ffprobe -v error -show_entries stream=codec_name,width,height -of csv=p=0 \
     "rtmp://hangzhouwanget.hifleet.com:1935/HangZhouBridge/HangZhouBridgeNorth8_1"
   # 输出: h264,2560,1440 ✅
   ```

### 阶段 3：长测验证（进行中）

参照 `manual-long-run-guide.md` 执行 L1-L4 长测。

### 阶段 4：开机自启（待完成）

L4 长测通过后执行：

```bash
sudo systemctl enable hangzhouwan.target
```

### 阶段 5：Windows 保留期（待完成）

1. 双路稳定 24 小时后，Windows 保留 3-7 天
2. 期间持续监控 BM1684 稳定性
3. 任何异常可立即回切 Windows

### 阶段 6：最终停用 Windows（待完成）

1. 7 天无异常后，可停用 Windows 旧服务
2. 停用前确认：
   - BM1684 24 小时无异常
   - 回滚演练已执行
   - 诊断报告正常
3. **停用 Windows 需人工确认，不自动执行**

---

## 回切流程

### BM1684 回滚到上一版本

```bash
sudo bash /opt/hangzhouwan/current/tools/release/rollback_release.sh
sudo systemctl restart hangzhouwan.target
```

### 回切到 Windows

1. 在 Windows 机器上启动旧服务，推流到正式 RTMP 地址
2. 停止 BM1684：
   ```bash
   sudo systemctl stop hangzhouwan.target
   ```
3. 确认 Windows 推流正常，正式地址可拉流
4. BM1684 保留待修复

---

## 验收清单

- [x] A 路推流正式地址正常（fps、检测率）
- [x] B 路推流正式地址正常（fps、检测率）
- [x] 坐标预测正常（COORD_ONLY，真实经纬度）
- [x] MQTT AIS 数据接收正常（缓存 300+ 艘船）
- [ ] 真实 AIS 人工样本验证（A/B 各 ≥20 个匹配样本，需有船经过时采集）
- [ ] 24 小时长测无异常
- [ ] Windows 回切演练成功
- [ ] 开机自启已启用

---

## 禁止事项

- ❌ 在未完成 24 小时长测前 `systemctl enable`
- ❌ BM1684 和 Windows 同时推流到同一正式地址（双推冲突）
- ❌ 跳过 24 小时观察窗口
- ❌ 不执行回滚演练就停用 Windows
