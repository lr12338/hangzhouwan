# Windows 服务灰度替换方案

> 目标：将 Windows 旧服务逐步替换为 BM1684 双路检测系统。
> 红线：不直接双路切换；不自动停止 Windows 旧服务；失败立即回切。

## 替换原则

1. **Windows 继续发布正式流**，BM1684 先发布到灰度地址
2. **灰度对比** 至少 24 小时后再切换
3. **逐路切换**：先 A 路，观察后再 B 路
4. **Windows 保留 3-7 天** 作为回切兜底
5. **失败立即回切**

## 前置门禁

在开始灰度前，必须全部满足：

- [ ] systemd 实际启用业务融合（`--enable-business`）
- [ ] `application.yaml` 为唯一权威配置
- [ ] 推流画面能区分 AIS 匹配状态（绿/黄/红）
- [ ] JSONL 格式合法（`hzwctl` + 单元测试验证）
- [ ] AIS 证据完整（真实坐标，非 0）
- [ ] release 可校验（SHA256）
- [ ] 原子升级/回滚成功
- [ ] preflight 全通过
- [ ] 300 秒灰度通过（T8）
- [ ] 30 分钟通过（L1）
- [ ] 2 小时通过（L2）
- [ ] 8 小时通过（L3）
- [ ] 24 小时通过（L4）
- [ ] 回滚演练成功

## 灰度步骤

### 阶段 0：准备

1. 构建 release 并激活：
   ```bash
   bash tools/release/build_release.sh
   bash tools/release/activate_release.sh <release_dir>
   ```
2. 配置灰度 RTMP 输出地址（与 Windows 正式流不同）：
   - `STREAM_A_OUTPUT_URL` 指向灰度地址 A
   - `STREAM_B_OUTPUT_URL` 指向灰度地址 B
3. 预检通过：`hzwctl preflight`
4. 回滚演练：`bash tools/release/rollback_release.sh`

### 阶段 1：双发对比（24 小时）

1. Windows 继续发布正式流
2. BM1684 发布到灰度地址
3. 同时观看/录制两路输出，对比：
   - 检测框位置和数量
   - AIS 匹配结果
   - fps 和延迟
   - 丢帧率
4. 对比 JSONL 与 Windows 旧版输出
5. **24 小时无异常后进入阶段 2**

### 阶段 2：切换 A 路

1. 将 A 路正式 RTMP 地址切换到 BM1684
2. Windows 旧版 A 路降级为备份（不停止）
3. 观察至少 24 小时：
   - fps 稳定
   - 无异常重连
   - JSONL 合法
   - 业务融合正常
4. **异常立即回切 A 路到 Windows**

### 阶段 3：切换 B 路

1. A 路稳定 24 小时后，切换 B 路
2. 同样观察 24 小时
3. **异常立即回切 B 路**

### 阶段 4：Windows 保留期

1. 双路均切换到 BM1684 后，Windows 保留 3-7 天
2. 期间持续监控 BM1684 稳定性
3. 任何异常可立即回切 Windows

### 阶段 5：最终停用 Windows

1. 7 天无异常后，可停用 Windows 旧服务
2. 停用前确认：
   - BM1684 24 小时无异常
   - 回滚演练已执行
   - 诊断报告正常
3. **停用 Windows 需人工确认，不自动执行**

## 回切流程

### BM1684 回滚到上一版本

```bash
bash tools/release/rollback_release.sh
sudo systemctl restart hangzhouwan.target
```

### 回切到 Windows

1. 将正式 RTMP 地址 DNS/配置切回 Windows
2. 确认 Windows 旧服务正常运行
3. 停止 BM1684（可选）：
   ```bash
   sudo systemctl stop hangzhouwan.target
   ```

## 禁止事项

- ❌ 在没有灰度对比时直接双路切换
- ❌ 自动停止 Windows 旧服务
- ❌ 跳过 24 小时观察窗口
- ❌ 不执行回滚演练就切换

## 验收清单

完成灰度替换后，确认：

- [ ] A 路符合目标（fps、检测率、AIS 匹配）
- [ ] B 路达到批准阈值
- [ ] 真实 MQTT 消息验证
- [ ] 真实 AIS 人工样本验证（A/B 各 ≥20 个匹配样本）
- [ ] Windows 回切演练成功
- [ ] 24 小时无异常
