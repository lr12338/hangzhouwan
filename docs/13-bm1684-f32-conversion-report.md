# 13 · BM1684 F32 bmodel 转换报告

检查时间：2026-07-21。

## 已确认输入

| 项目 | 值 |
|---|---|
| 分支 | `feat/bm1684-edge-deployment` |
| ONNX | `hangzhouwan_beishang/weights/best.onnx` |
| ONNX SHA256 | `101f8e19c680eb4fd2446521f983b11ada35052bce56f30c2ddcf5df64894f92` |
| 输入 | `images`，FLOAT，原始动态 `[batch,3,height,width]` |
| 转换输入 | `[1,3,640,640]` |
| 输出 | `output`，无内置 NMS |
| 目标 | `BM1684`，F32 |

`tools/inspect_onnx.py` 已在本 x86 工作区复核上述模型结构。`tools/convert_model/convert_bmodel.sh` 已修正为 `MODE=f32` 默认流程：执行 `model_transform.py`、`model_deploy.py --quantize F32 --processor BM1684`，并要求工具链以一张本地图片执行 ONNX 参考输出和 MLIR/F32 原始 Tensor 比较。它不会进入 INT8，也不含 FP16 分支。

## 真实执行状态

转换未开始，未生成 bmodel。原因如下：

1. Docker daemon 在主机侧正常，但官方仓库 Registry 查询 `sophgo/tpuc_dev:v3.4` 时访问 Docker Hub 超时；因此无法记录可拉取镜像的 digest，也不能使用不可追溯的 `latest`。
2. 真实工作区同步副本没有 `testdata/calibration/`。F32 仍需要至少一张本地验证图片；脚本会在该目录中选择第一张图片，或要求显式设置 `TEST_IMAGE`。
3. 未提供 BM1684 板端 IP 或可用 SSH 配置，故未执行要求的真实工作区 `rsync`，也没有交付复制动作。

因此没有 `artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel`、`conversion.log`、模型清单、SHA256 或交付包；没有伪造模型查看、数值相似度或板端加载结果。

## 继续条件

1. 恢复到官方 Sophgo TPU-MLIR Registry 或提供由 Sophgo 发布、带固定 digest 的离线镜像。
2. 提供板端 IP/SSH 连通性后，以 `rsync` 同步真实开发工作区，并确认校准图片和模型 SHA256。
3. 在 Docker 容器中确认 `model_transform.py`、`model_deploy.py` 和 `model_tool` 支持 `BM1684`，然后运行：

```bash
MODE=f32 bash tools/convert_model/convert_bmodel.sh
```

最终结论：**磁盘或 Docker 环境阻塞**。
