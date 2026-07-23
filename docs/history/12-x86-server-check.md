# 12 · x86 服务器关键检查

检查时间：2026-07-21。检查范围仅限 BM1684 F32 bmodel 转换的离线镜像、主机空间和模型资产；未执行转换、INT8 或板端操作。

| 项目 | 实测 | 结论 |
|---|---|---|
| 离线归档 | `/home/huangchao/tpu-mlir-offline/tpuc_dev_v3.4.tar.gz`，2,124,689,249 字节 | 已校验 |
| 归档 SHA256 | `d83e2cb55bc279076e516597363429c9bab76aae7999260046a786220b4819ac` | 与本次任务给出的本地完整性基线一致；不是官方公布校验值的声明 |
| 导入镜像 | `sophgo/tpuc_dev:v3.4`，本地别名 `local/tpuc_dev:v3.4` | 导入成功 |
| 镜像 ID / 大小 | `sha256:d73afc9614a7e78e69dca61a9b7ac84ee9598c942571ea557c603850c5df59a1` / 7.14 GB | 已记录 |
| 镜像属性 | `amd64` / `linux`，创建时间 `2025-04-11T13:11:38.79942532Z` | 通过 |
| 导入后根分区可用空间 | `docker load` 结束后立即观测为 9.4 GB；复核时为 16 GB | 曾低于下限；当前恢复到下限以上，后续执行前仍须复核 |
| 容器工具 | 未找到 `model_transform.py`、`model_deploy.py`、`model_runner.py`、`npz_tool.py`、`model_tool` 或 `envsetup.sh` | 工具链不可用 |
| 模型资产 | `weights/best.onnx` 不存在；发现不同路径的 `hangzhouwan_beishang/weights/best.onnx`（24,133,596 字节） | 不满足本次受控输入路径 |
| 测试图片 | 未找到 JPG/JPEG/PNG；仅有 `testdata/test.mp4` | 不满足验证输入条件 |
| 抽帧工具 | 主机和容器均未找到 `ffmpeg` / `ffprobe` | 未安装额外依赖 |

## 结论

离线镜像文件完整且 Docker 导入成功。`docker load` 刚结束时可用空间曾为 9.4 GB，之后复核为 16 GB；任何后续转换开始前都必须再次确认至少保留 10 GB。镜像中未发现 TPU-MLIR 转换、运行、比较或模型查看工具，以及可加载这些工具的环境脚本。受控路径 `weights/best.onnx` 与测试图片都不存在。

因此本次没有运行 `model_transform.py`、`model_deploy.py`、`model_runner.py`、INT8、板端加载或网络流。未删除任何 Docker 镜像或执行 Docker 清理。
