# 模型与测试资产清单

> 最后更新：2026-07-23

本文件记录仓库中所有模型、权重和测试资产的元数据。
大文件被 `.gitignore` 排除，不入库，仅本地存在。

## 生产模型（入库）

| 逻辑名称 | 文件名 | 用途 | 来源 | 目标平台 | 大小 | SHA256 | 进入 Release | 加载代码 | 替换方法 |
|---|---|---|---|---|---|---|---|---|---|
| YOLOv7 ship bmodel | `artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel` | 船舶检测推理 | TPU-MLIR F32 转换 | BM1684 | 24,428,544 B | `d1c295c5...` | 是 | `src/inference/bmrt_detector.cpp` | x86 运行 `tools/convert_model/convert_bmodel.sh` |

## 坐标模型（外部，不入库）

| 逻辑名称 | 文件名 | 用途 | 来源 | 目标平台 | 大小 | SHA256 | 进入 Release | 加载代码 | 替换方法 |
|---|---|---|---|---|---|---|---|---|---|
| A 路坐标模型 | `weights/0121_random_forest_model.pkl` | A 路 pixel→world 坐标预测 | sklearn RandomForest 训练 | Python 3.8 (sklearn) | 7,595,545 B | `d25e7d0e...` | 是（build_release.sh 复制） | `services/business_enrichment/coordinate/sklearn_predictor.py` | 重新训练后替换文件 |
| B 路坐标模型 | `weights/beishang_x-l.pkl` | B 路 pixel→world 坐标预测 | sklearn 训练 | Python 3.8 (sklearn) | 2,147,673 B | `f85934c4...` | 是（build_release.sh 复制） | 同上 | 同上 |

## 开发用模型（外部，不入库，非生产热路径）

| 逻辑名称 | 文件名 | 用途 | 来源 | 大小 | SHA256 | 进入 Release |
|---|---|---|---|---|---|---|
| YOLOv7 原始 ONNX | `weights/best.onnx` | ONNX 审计与 bmodel 转换源 | PyTorch 导出 | 24,133,596 B | `101f8e19...` | 否 |
| PyTorch 权重 | `weights/beet0110.pt` | 原始训练权重（历史参考） | 训练产出 | 74,775,738 B | `3ea33fd3...` | 否 |

## 测试资产（入库）

| 逻辑名称 | 文件名 | 用途 | 大小 | SHA256 | 验证方式 |
|---|---|---|---|---|---|
| 测试视频 | `testdata/test.mp4` | 离线推理与管线测试 | 16,741,284 B | `e6ef38f3...` | `tests/integration/test_assets.py` |

## 物理路径说明

当前保留 `artifacts/`（bmodel）和 `weights/`（PKL/ONNX/PT）的现有物理路径。
路径统一（合并为 `assets/models/`）列为后续任务，本轮不执行以降低风险。

`build_release.sh` 从以下路径复制模型到 Release：
- `artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel` → `models/`
- `weights/0121_random_forest_model.pkl` → `models/`
- `weights/beishang_x-l.pkl` → `models/`
