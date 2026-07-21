# 14 · BM1684 F32 交付指南

当前没有可交付 bmodel。2026-07-21 的离线检查中，`docker load` 结束后根分区曾为 9.4 GB，最终复核为 16 GB；镜像内仍没有可用 TPU-MLIR 工具，受控模型 `weights/best.onnx` 和测试图片也缺失。因此未生成交付压缩包，未执行 SCP 或板端验证。

仅当 F32 转换和 x86 原始输出 Tensor 数值比较均真实通过后，才能生成下列包：

```text
artifacts/bm1684-f32/
├── yolov7_ship_1684_f32.bmodel
├── sha256sum.txt
├── model-info.txt
├── model-manifest.json
├── conversion.log
└── numerical-validation.log
```

模型清单必须说明目标为 `BM1684`、精度为 `F32`、输入为 `images [1,3,640,640]`、输出为 `output`、不含 NMS，以及 x86 转换和数值比较均通过、板端加载尚未验证。不得包含 ONNX、PKL、校准图片、NPZ、Docker 镜像、凭据或 Git 元数据。

```bash
tar -czf artifacts/yolov7_ship_1684_f32_delivery.tar.gz \
  -C artifacts/bm1684-f32 \
  yolov7_ship_1684_f32.bmodel sha256sum.txt model-info.txt \
  model-manifest.json conversion.log numerical-validation.log
sha256sum artifacts/yolov7_ship_1684_f32_delivery.tar.gz \
  | tee artifacts/yolov7_ship_1684_f32_delivery.tar.gz.sha256
```

板端 IP 尚未提供，不影响本次停止结论；在用户明确提供板端信息且 x86 验证通过前，不应执行复制、加载、单图推理、RTSP、MQTT、RTMP 或 systemd 修改。
