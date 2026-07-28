# 杭州湾单机工业生产加固

本 Release 将视频热路径、事件数据和恢复控制面分离。生产输出固定为
1280×720、10fps、1200kbps、GOP 20；检测、禁区和坐标计算继续使用
2560×1440 原始坐标，绘制完成后由 BMCV VPP 缩放为 YUV420P，再交给
`h264_bm` 编码。

## 数据保留与磁盘保护

- 每路事件写入 `/data/hangzhouwan/events/stream_<id>.current.jsonl`。
- 写入队列为 4MB，5 秒 `fdatasync`，阻塞时丢弃最旧记录并累计计数。
- 50MB 或 1 小时轮转，gzip 压缩，保留 7 天。
- `/data` 少于 2GB告警；少于 1GB停写事件，恢复到 2GB才解除。
- `/data` 不可用时视频继续，事件不得回退到根分区。
- 健康接口的 `storage`、`event_writer` 和 `active_alerts` 是磁盘处置依据。

## 健康和就绪

`hzwctl health --json` 只输出一行 JSON。新增字段包括
`business_link`、`enrichment_mode`、`reconnects_1h`、`storage`、
`event_writer`、`resource` 和 `active_alerts`。无检测目标时
`enrichment_mode=PENDING`，只要 Sidecar 响应正常就不会判为故障。

严格门禁：

```bash
/opt/hangzhouwan/current/bin/hzwctl wait-health --timeout 180
```

门禁要求 Sidecar、A/B RTSP、A/B RTMP、输出不低于 7fps、推理不低于
4fps、滑动一小时重连不超过 2 次，且事件存储未进入保护状态。

## 恢复策略

监督器每 30 秒检查一次。`DEGRADED` 只告警；连续 3 次 `FAILED` 或健康
Socket 失联 90 秒才允许采集诊断并重启一次 Video。恢复冷却 30 分钟，
24 小时最多 2 次。上游端口不可达、磁盘临界或维护锁存在时禁止恢复。

每日 03:30 维护使用 `/run/hangzhouwan/maintenance.lock`，开机或 Video
运行不足 30 分钟时跳过补执行。顺序为诊断、前检、Business、Sidecar、
Video、三次严格健康确认，总上限 180 秒；失败只告警，不循环重启。

## 权限和发布

- Release 位于 `/data/hangzhouwan/releases`，由 `root:root` 拥有且只读。
- Python 依赖复制到 Release venv，设置 `PYTHONNOUSERSITE=1`，不得出现
  `/home` 或用户 site-packages。
- Video 与 Business 使用独立系统账户，共享 `hangzhouwan` 运行组。
- Video 仅获准访问 BM1684 所需字符设备；两服务均为空能力集、禁 core、
  限制 Tasks/RSS，并启用只读系统目录。
- 安装与激活前必须验证所有文件 SHA256、systemd unit 和权限；激活失败
  原子回滚至 `previous` 并保留诊断。

## 现场遗留项

B 路相机/链路高频断流是独立现场问题。监督器只记录重连率和告警，不以
频繁服务重启掩盖。72 小时签收前必须结合摄像机响应时间、网卡丢包、
RTSP 错误与相机固件完成排查。
