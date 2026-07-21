# Agent 接手提示（AGENT_HANDOFF）

## 当前状态：2026-07-21

项目根目录为 `/home/linaro/hangzhouwan-orign/hangzhouwan`，分支为 `feat/bm1684-edge-deployment`。目标芯片固定为 BM1684（`chipid=0x1684`），已完成 F32 bmodel 板端验证、单图 C++ 推理 PoC、阶段4 单路硬件视频管线功能与 BMCV 性能优化。
短时 300s 验证通过（10fps）；30min/2h 长时门禁待人工执行。阶段4.2 单路 RTSP 输入代码+单元测试+连接路径验证完成，实流/长时待人工。

## 已完成

### 阶段2：F32 bmodel 转换与 x86 验证
- F32 bmodel 已生成：`artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel`（24,428,544 字节）
- Git 提交 `9255f27`（bmodel 已跟踪，`git ls-files` 确认）
- SHA256：`d1c295c504888541c27e20fda500976725b9d13b6ef5c91842f247f52c31cfcd`
- ONNX/MLIR 172 个 Tensor 数值验证通过；bmodel cmodel 比较通过

### 阶段2B：板端 bmodel 真实加载
- `bmrt_test` 加载成功（退出码 0）
- `bmrt_load_test` 编译运行成功（退出码 0）
- 网络 `yolov7_ship`：输入 `images [1,3,640,640] FLOAT32`，输出 `output_Concat [1,25200,6] FLOAT32`
- 输出名动态读取（非硬编码 `output`）
- 无兼容性错误，TPU 资源正常释放

### 阶段3：单图 C++ 推理 PoC
- 代码结构：`include/inference/` + `src/inference/` + `include/image_io/` + `src/image_io/`
- 预处理：libjpeg 解码(RGB) → 直接 resize 640×640 → /255 → NCHW FLOAT32（与 Python 一致）
- 后处理：obj_conf 过滤 → score 过滤 → 坐标映射 → NMS（与 detector.py 一致）
- 单元测试：10 项全部通过
- 单图推理：frame_30.jpg 检测到 2 艘船（预处理 56ms，推理 16ms，后处理 0.6ms）
- 100 次重复推理：平均 15.99ms，框数稳定，无内存泄漏
- Python 测试：26 项全部通过

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure

# 单图推理
LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib \
  ./build/single_image_infer \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --image testdata/model_test/frame_30.jpg \
  --output result.jpg --json result.json \
  --conf 0.1 --iou 0.1 --device 0

# 100 次重复推理
LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib \
  ./build/single_image_infer \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --image testdata/model_test/frame_30.jpg \
  --conf 0.1 --iou 0.1 --device 0 --loop 100
```

## 精度声明

板端 F32 单图推理、后处理和绘框功能已通过。**最终检测框精度仍需与同图 ONNX 基线 JSON 对照**。当前无 x86 基线 JSON。

## 阶段4：单路硬件视频管线（🔶 功能通过+300s验证通过，30min/2h 待人工执行）

```bash
LD_LIBRARY_PATH=/opt/sophon/sophon-ffmpeg_0.8.0/lib:/opt/sophon/libsophon-0.4.9/lib \
  ./build/single_video_infer \
  --input testdata/test.mp4 \
  --output artifacts/stage4/output_single_stream.mp4 \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --device 0 --decoder h264_bm --encoder h264_bm \
  --output-fps 10 --inference-fps 5 --queue-size 1 --loop 1 \
  --preprocess cpu --draw-mode bmcv
```

推荐配置 `--preprocess cpu --draw-mode bmcv`：CPU 预处理保证检测正确性，BMCV 绘制消除 sws 往返瓶颈（132ms -> 6ms/帧），输出达 10fps。

稳定性测试：
```bash
./tools/video_inference/stability_test.sh 10   # 10 秒功能
./tools/video_inference/stability_test.sh 60   # 60 秒性能
./tools/video_inference/stability_test.sh 300  # 300 秒短时稳定性
./tools/video_inference/stability_test.sh 30   # 30 分钟（人工）
./tools/video_inference/stability_test.sh 120  # 2 小时（人工）
```

稳定性测试结果：
- 10s 功能 ✅ 通过（CPU+CPU / BMCV+BMCV / BMCV+none 三模式）
- 60s 性能 ✅ 通过（CPU+BMCV 10.02fps / BMCV+BMCV 10.03fps）
- 300s 短时稳定性 ✅ 通过（10.003fps，退出码 0，RSS +356KB，P95=150ms 稳定，无残留）
- 30min 中等 ⏳ 待人工执行（详见 `docs/20-stage4-manual-long-run-guide.md`）
- 2h 最终门禁 ⏳ 待人工执行

## 阶段4.2：单路 RTSP 输入到本地文件（🔶 代码+单测+连接路径验证通过，实流/长时待人工）

扩展 `SophonVideoSource` 支持 RTSP：`open_rtsp`（`rtsp_transport`+`stimeout` 经 AVDictionary，参数来自板端 `ffmpeg -h demuxer=rtsp` 实测）、中断回调（`stop_requested_` 使 open/read 可被信号/限时中断）、`read()` 内受控重连（指数退避，封顶，受 `--rtsp-max-reconnect` 约束）。

安全：`--input-env HZW_TEST_RTSP_URL` 环境变量输入（URL 不入命令行/日志/Git）；`redact_url_credentials` 脱敏（`rtsp://user:***@host`）；提交前 `python3 tools/redact_secrets.py --scan .`。

```bash
export HZW_TEST_RTSP_URL='rtsp://用户名:your_password@测试地址:554/路径'   # 板端私下设置，勿写入文件
./build/single_video_infer \
  --source-type rtsp --input-env HZW_TEST_RTSP_URL \
  --output artifacts/stage4_2/rtsp_60s.mp4 \
  --bmodel artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel \
  --device 0 --decoder h264_bm --encoder h264_bm \
  --rtsp-transport tcp --rtsp-stimeout-us 5000000 --rtsp-max-reconnect -1 \
  --source-fps 20 --output-fps 10 --inference-fps 5 --queue-size 1 \
  --preprocess cpu --draw-mode bmcv --max-seconds 60
```

验证状态：
- `ctest` 8 项 + Python 29 项全通过（新增 `test_rtsp_source_options`、`test_output_pts`、`redact_secrets --scan`）。
- 20s 本地文件回归通过（硬件链路未回退）。
- 受控无效地址验证：连接超时（`stimeout`）、SIGTERM/SIGINT 中断阻塞（`Immediate exit requested`）、凭据不泄漏、无残留。
- 待人工：RTSP 实流解码、中途断线重连、30min/2h 门禁（见 `docs/21`、`docs/22`）。

限制：板端 Sophon-FFmpeg RTSP muxer 不支持 listen，无可用 RTSP 服务端工具，无法自建本地中继自测。

## 待完成

- 与 x86 ONNX 基线 JSON 进行精度对照
- 阶段4：30min/2h 长时稳定性门禁人工执行（详见 `docs/20`）
- 阶段4.2：RTSP 实流解码 + 中途断线重连 + 30min/2h 门禁人工执行（详见 `docs/22`）
- 后续：INT8 量化、AIS 坐标映射、双路 Pipeline、部署

## 执行红线（始终遵守）

禁止：安装 TPU-MLIR、Python SAIL、Torch/CUDA、修改 libsophon、INT8 转换、连接生产 RTSP/MQTT/RTMP/启动双路/INT8、修改 systemd、硬编码输出名为 `output`、伪造测试结果。
