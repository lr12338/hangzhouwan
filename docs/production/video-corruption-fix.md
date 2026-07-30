# 1280×720 推流画面乱码修复与上线门禁

> 状态（2026-07-30）：`video-frame-fix-20260730-9c6cec3` 已激活生产，
> previous 为 `industrial-20260728-fdb878c`。A/B 正式 RTMP 软件解码验证
> 通过；仍需完成 2-4 小时及建议 24 小时长稳观察。

## 故障结论

生产链路在 `2560×1440 -> BMCV 1280×720 -> h264_bm` 后出现随机彩纹。
故障不在“配置成 720p”本身，而在缩放结果交给编码器时的内存契约：真实
像素位于 BMCV 设备图像，旧代码同时提交未填充的主机平面，并用
`data[4..6]` 手工描述设备地址。该帧既不是可靠的标准主机帧，也不满足
可验证的 Sophon DMA 帧契约。

修复后的链路为：

```text
2560×1440 解码/绘制
  -> BMCV VPP 缩放为 1280×720 YUV420P
  -> bm_image_copy_device_to_host
  -> 标准主机 AVFrame
  -> h264_bm (is_dma_buffer=0)
  -> RTMP 1280×720
```

编码器会校验输入的宽、高、像素格式和平面。禁止只修改编码器/FLV 元数据
而继续传入 2560×1440 像素，也禁止恢复未填充的占位缓冲或伪 DMA 指针。

## 已完成验证

- C++ 构建成功，BMCV 单元测试确认主机 Y/U/V 平面有效。
- 960×544 文件经 BMCV 放大到 1280×720、`h264_bm` 编码 80 帧，软件
  解码无错误；编码前帧与解码帧 SSIM 为 `0.990724`。
- A 路真实 2560×1440 RTSP 缩放并本地编码 5 秒，输出 48 帧，平均编码
  约 8.46ms，软件解码画面正常。
- Python 测试 `193 passed`；CTest `17/17 passed`（含稳定性脚本）。

生产激活补充验证：Release 校验 10/10、预检 46/46、严格健康门禁、60 秒
smoke 和 PID 来源核验通过；A/B 编码前帧均为 1,382,400 字节，画面正常；
A/B 正式 RTMP 软件解码抓帧成功，连续 20 秒解码返回 0 且错误计数为 0。
清除诊断变量并再次重启 Video 后，两路 RTMP 抓帧仍成功。

B 路因摄像机输入间隔抖动仍可能低于 9fps并使总体状态 DEGRADED；本次复验
中其输入间隔 P99 一度达到 1.49 秒。该问题与画面乱码修复分开跟踪。

## 维护窗口验证

1. 为候选配置临时 RTMP Key，不得覆盖正式流；保留已知可用的 previous。
2. 在 `/etc/hangzhouwan/video.env` 临时加入：

   ```bash
   HZW_PREENCODE_DUMP_DIR=/data/hangzhouwan/monitor
   ```

3. 激活候选并启动 Video。日志中的编码器启动行必须显示缩放路为
   `input=host`。每路会生成一次性文件：
   `preencode_A_1280x720.yuv`、`preencode_B_1280x720.yuv`。
4. 转换编码前帧：

   ```bash
   ffmpeg -f rawvideo -pixel_format yuv420p -video_size 1280x720 \
     -i /data/hangzhouwan/monitor/preencode_A_1280x720.yuv \
     -frames:v 1 /data/hangzhouwan/monitor/preencode_A_1280x720.png
   ```

5. 用软件解码器分别从 A/B 临时 RTMP 流保存帧，并持续解码至少 5 分钟。
6. 对比原始 RTSP、编码前 PNG、RTMP 解码帧。内容必须一致，允许缩放和
   叠框差异，不允许随机彩纹、绿屏、黑屏、平面错位或连续解码错误。
7. A 路持续输出至少 9fps；双路短测通过后再按维护窗口批准切换正式 Key。
8. 验证后从 `video.env` 删除 `HZW_PREENCODE_DUMP_DIR` 并重启 Video，避免
   后续重启反复覆盖诊断证据。

## 回滚条件

任一编码前帧已损坏、RTMP 软件解码异常、A 路持续低于 9fps、编码器输入
契约报错或服务状态 `FAILED`，立即执行 `rollback_release.sh`。B 路 RTSP
重连和 PPS 缺失继续作为独立上游问题记录，不应据此否定画面内存修复；
但 B 路未能取得可比较的完整帧时，双路 G8 门禁仍为未通过，不能签收。
