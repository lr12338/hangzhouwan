# 依赖分类说明

历史仓库的 Windows conda freeze（`hangzhouwan_beishang/requirements.txt`、
`环境requirements.txt`、`yolov7_requirements.txt`，均已移除）存在大量冲突 pin（numpy / torch / paho_mqtt / pyais /
Pillow 重复且版本不一致）与 Windows conda freeze（含 `file:///C:/...` 本地路径），
**不能作为 BM1684 工控机的安装依据**。本目录按用途重新分类。

## 清单

| 文件 | 用途 | 是否在工控机安装 |
|---|---|---|
| `baseline-x86.txt` | x86 开发机复现原版 ONNX/PyTorch 推理与坐标模型验证 | **否**（仅 x86） |
| `tools.txt` | 配置校验 / 脱敏 / 静态检查 / 离线测试 | 当前已满足，按需补充 |

## BM1684 生产环境（阶段1只写说明，不生成 pip 安装清单）

正式 BM1684 热路径预期为 **C++ 全栈**，不依赖 Python 推理框架：

- C++ BMRuntime（`libbmrt.so`，libsophon 0.4.9 LTS）
- BMCV / BMVideo / BMVPU / VPP
- Sophon-FFmpeg 0.8.0（硬解 / 硬编 `h264_bm` / `h265_bm`）
- Sophon-OpenCV 0.8.0（C++ 接口）
- MQTT C/C++ 客户端库
- YAML 配置库（C++，如 yaml-cpp）

## 禁止项

**不得**将下列依赖列入 BM1684 生产环境：

- `torch` / `torchvision`（PyTorch）
- `onnxruntime-gpu`（CUDA 后端）
- 任何 CUDA / cuDNN 相关包
- `yolov7==0.0.1`（原 Windows conda freeze 中的脏包）

## 工控机当前 Python 实测可用

`Python 3.8.2`，已装：`numpy`、`PyYAML`、`psutil`。
未装（按计划本阶段不装）：`cv2`(Sophon-OpenCV Python)、`onnx`、`onnxruntime`、
`joblib`、`scikit-learn`、`pandas`、`Pillow`、`jsonschema`、`pytest`、
`paho-mqtt`、`pyais`。

阶段1离线测试已基于 **标准库 unittest** 编写，无需 pip 安装即可运行：
`python3 tests/run_tests.py`。
