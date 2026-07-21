# 14 · BM1684 F32 交付指南

本文件定义 F32 转换成功后的受控交付方式。当前尚未生成任何交付包。

## 生成条件

仅当下列文件真实存在且验证通过时才允许打包：

```text
artifacts/bm1684-f32/
├── yolov7_ship_1684_f32.bmodel
├── conversion.log
├── toolchain-version.txt
├── model-manifest.json
└── sha256sum.txt
```

包中仅包含上述 F32 bmodel、SHA256、模型清单、工具链版本、转换日志和板端验证说明。不得包含 ONNX、`.pkl`、校准图片、Docker 镜像、Git 元数据、生产配置或凭据。

```bash
test -s artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel
tar -C artifacts/bm1684-f32 -czf artifacts/yolov7_ship_1684_f32_delivery.tar.gz \
  yolov7_ship_1684_f32.bmodel conversion.log toolchain-version.txt model-manifest.json sha256sum.txt
sha256sum artifacts/yolov7_ship_1684_f32_delivery.tar.gz
```

## 板端复制

使用新的、不可覆盖的目录，日期或 x86 Git SHA 作为目录名。复制前先确认目录不存在，复制后在板端校验 SHA256；不要修改 systemd。

```bash
DEST=/data/hangzhouwan-delivery/$(date +%F)-<git-sha>
ssh linaro@<BM1684-IP> "test ! -e '$DEST' && mkdir -p '$DEST'"
scp artifacts/yolov7_ship_1684_f32_delivery.tar.gz linaro@<BM1684-IP>:"$DEST/"
ssh linaro@<BM1684-IP> "cd '$DEST' && sha256sum yolov7_ship_1684_f32_delivery.tar.gz"
```

## 板端验证

解包后将 bmodel 放入板端工作区 `weights/`。以下命令只用于加载和原始模型验证，不能作为最终检测框精度结论：

```bash
/opt/sophon/libsophon-current/bin/bmrt_test \
  --bmodel weights/yolov7_ship_1684_f32.bmodel \
  --devid 0

LD_LIBRARY_PATH=/opt/sophon/libsophon-0.4.9/lib \
  ./tools/image_inference/bmrt_load_test weights/yolov7_ship_1684_f32.bmodel
```

执行前需在板端以实际 `--help` 输出核对参数；x86 侧不得声称这些命令已经成功。
