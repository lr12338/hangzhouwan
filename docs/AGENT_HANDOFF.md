# Agent 接手提示（AGENT_HANDOFF）

## 当前状态：2026-07-21

项目根目录为 `/home/huangchao/hangzhouwan/hangzhouwan`，分支为 `feat/bm1684-edge-deployment`。目标芯片固定为 BM1684（`chipid=0x1684`），本任务路线仅为 F32，禁止 BM1684X、FP16、INT8、板端加载、网络流和 systemd 修改。

离线归档 `/home/huangchao/tpu-mlir-offline/tpuc_dev_v3.4.tar.gz` 已验证本地 SHA256：

```text
d83e2cb55bc279076e516597363429c9bab76aae7999260046a786220b4819ac
```

镜像已导入为 `sophgo/tpuc_dev:v3.4`，并标记 `local/tpuc_dev:v3.4`。其 ID 为 `sha256:d73afc9614a7e78e69dca61a9b7ac84ee9598c942571ea557c603850c5df59a1`，架构为 `amd64`，系统为 `linux`。

## 现有阻塞

1. `docker load` 后立即根分区可用空间曾为 9.4 GB，最终复核为 16 GB；每次转换开始前仍须确认至少保留 10 GB。
2. 镜像内没有发现 `model_transform.py`、`model_deploy.py`、`model_runner.py`、`npz_tool.py`、`model_tool` 或 `envsetup.sh`，也没有发现可按官方说明安装的 TPU-MLIR wheel。
3. 受控模型路径 `weights/best.onnx` 不存在。仓库中另有 `hangzhouwan_beishang/weights/best.onnx`，但不得替代受控输入；须由用户手动复制。
4. 没有测试图片，只有 `testdata/test.mp4`；主机和容器均无 `ffmpeg`。

因此没有 bmodel、数值比较、模型清单、交付包或板端结果。任何接手者都不得伪造这些结果。

## 已复核的转换约束

`tools/convert_model/convert_bmodel.sh` 通过 `bash -n`，默认 F32 分支使用 `set -euo pipefail`、`--processor BM1684`、`--input_shapes [[1,3,640,640]]`、输入 `images`、输出 `output` 和 `--quantize F32`。INT8 仅会在显式 `MODE=int8` 时进入，本任务不得执行。原 `detector.py` 使用 BGR 转 RGB、直接 resize 640x640、`/255`、NCHW，无 letterbox。

## 解除后顺序

先满足空间、官方工具、`weights/best.onnx` 和测试图片四项条件；随后只运行 F32 转换、`model_tool --info` 和 x86 原始输出 Tensor 数值比较。比较通过后生成交付包。不要自动继续 INT8、板端、单图 C++ 推理或任何流服务。

修改后运行 `python3 tests/run_tests.py`、`git diff --check`，只本地提交，不执行 push。新增日志和报告使用中文。
