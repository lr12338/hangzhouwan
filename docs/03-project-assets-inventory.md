# 03 · 项目资产盘点（基于本地完整 Git 克隆）

> 导航：本文件为阶段1资产盘点，依据本地完整克隆仓库真实检查结果，不依据先前报告。

| 项 | 内容 |
|---|---|
| 盘点日期 | 2026-07-20 |
| 仓库根 | `/home/linaro/hangzhouwan-orign/hangzhouwan` |
| 远程 | `https://github.com/lr12338/hangzhouwan.git` |
| 分支 | `feat/bm1684-edge-deployment`（由 `main` 创建） |
| `main` HEAD | `56d380f3f25a49dfaa012946dd11d2c3a0fe7bef` |
| 盘点方式 | `find` / `git ls-files` / `git status --ignored` / `sha256sum` / `ffprobe`，只读 |

## 1. 项目结构概览

```
hangzhouwan/
├── .gitignore                         # 阶段1已重写（UTF-8，补齐忽略项）
├── hangzhouwan_beishang/              # 唯一业务目录（~87MB，含 weights）
│   ├── main.py / starter.py / starter_optimized.py   # 三个入口
│   ├── stream_handler{,_v2,_optimized}.py            # 三个流处理版本
│   ├── detector.py / detect.py / detector_pt.py(空)  # ONNX / PyTorch 检测
│   ├── demo.py / system_optimizer.py / fix_streaming_issues.py
│   ├── test_rtmp_connection.py
│   ├── requirements.txt / 环境requirements.txt        # 冲突 pin
│   ├── start_optimized.bat / start_system.bat / system_optimize.bat
│   ├── 优化部署指南.md
│   ├── output.txt                     # 80MB 运行时产物（已 untrack，含历史凭据）
│   ├── error.txt / rtmp_test_report.txt
│   ├── weights/simhei.ttf             # 中文字体（存在）
│   └── utils_demo/{config,find_ship,getais,getais_flask,method,plot}.py
├── testdata/test.mp4                  # 测试视频（存在，16MB）
├── nginx 1.7.11.3 Gryphon/            # Windows nginx-rtmp 源码树（~1.8MB，与迁移无关）
└── yolov7_requirements.txt            # Windows conda freeze（UTF-16，含 file:///C:/ 路径）
```

> 仓库总体积约 133MB，其中 `.git` 约 29MB（含 80MB output.txt 的 blob）、
> `hangzhouwan_beishang` 约 87MB（绝大部分为已 untrack 的 output.txt）。

## 2. 模型与资源资产表

| 资产 | 路径 | 大小 | SHA256 | Git 状态 | 是否可用 | 下一阶段用途 |
|---|---|---|---|---|---|---|
| 检测模型 best.onnx | `hangzhouwan_beishang/weights/best.onnx` | — | — | **缺失**（`.gitignore` 排除 `*.onnx`，未入库） | 否 | Stage2 转 bmodel（阻塞） |
| 坐标模型 A `0121_random_forest_model.pkl` | `hangzhouwan_beishang/weights/0121_random_forest_model.pkl` | — | — | **缺失**（`.gitignore` 排除 `*.pkl`） | 否 | A 路坐标映射（阻塞） |
| 坐标模型 B `beishang_x-l.pkl` | `hangzhouwan_beishang/weights/beishang_x-l.pkl` | — | — | **缺失**（`.gitignore` 排除 `*.pkl`） | 否 | B 路坐标映射（阻塞） |
| 中文字体 simhei.ttf | `hangzhouwan_beishang/weights/simhei.ttf` | 9,753,388 B (9.4M) | `aa4560dd8fe5645745fed3ffa301c3ca4d6c03cbd738145b613303961ba733b8` | 已跟踪 | 是 | 生产直接复用 |
| 测试视频 test.mp4 | `testdata/test.mp4` | 16,741,284 B (16M) | `e6ef38f30bb3db8b01807503066b80cb478e1bfc96151218854be32de6063604` | 已跟踪 | 是 | 离线基线素材 |
| nginx 模块测试图 bg.jpg | `nginx 1.7.11.3 Gryphon/nginx-rtmp-module/test/www/bg.jpg` | 16K | `37d6980df0031290a031bad0bdb4c16b120421f205b3ab7842f12f55be6d8cd1` | 已跟踪 | n/a | 非项目资产（nginx 模块自带） |

> 说明：`git status --ignored --short` 在工作区为空（即没有任何“被忽略且存在”的文件），
> 证实三个模型文件在本地克隆中确实不存在，并非“被忽略但仍在工作区”。

## 3. 测试视频可读性验证（Sophon-FFmpeg）

使用工控机自带 Sophon-FFmpeg 的 `ffprobe`（`/opt/sophon/sophon-ffmpeg-latest/bin/ffprobe`）探测：

| 项 | 值 |
|---|---|
| 编码 | h264 |
| 分辨率 | 960×544 |
| 帧率 | 20 fps |
| 时长 | 125.35 s |
| 码率 | ~1068 kbps |
| 大小 | 16,741,284 B |

> `ffprobe` 运行时输出 `BMvidDecCreateW5 ... vpu firmware ... chagall_dec.bin`，
> 表明 Sophon 硬件解码路径已激活，测试视频可被 Sophon-FFmpeg 解码。
> 注：Sophon-OpenCV 的 **Python** 绑定（`cv2`）在工控机未安装，
> 故 Python 侧 `cv2.VideoCapture` 读取测试未执行；C++ 侧 Sophon-OpenCV 0.8.0 可用。

## 4. 测试图片资产

项目无独立测试图片。仓库内仅 `nginx 1.7.11.3 Gryphon/nginx-rtmp-module/test/www/bg.jpg`
为 nginx-rtmp 模块自带测试素材，非项目资产。阶段1未伪造任何图片测试结果。

## 5. ONNX 结构审计条件

工控机当前未安装 `onnx` / `onnxruntime`（按计划本阶段不强制安装），
故无法在工控机对 `best.onnx` 做结构审计。且 `best.onnx` 本身缺失。
后续在 x86 环境补齐模型后，执行结构审计命令示例：

```bash
# x86 环境（已装 onnx）
python3 -c "import onnx; m=onnx.load('best.onnx'); onnx.checker.check_model(m); \
print([(i.name,i.type) for i in m.graph.input]); \
print([(o.name,o.type) for o in m.graph.output])"
```

## 6. 随机森林模型加载条件

两个坐标 `.pkl` 模型缺失，且工控机未安装 `joblib` / `scikit-learn`，
本阶段未反序列化任何 `.pkl`。后续确认为项目可信资产后，方可在隔离脚本中仅测试加载与输入输出形状。

## 7. 结论

- 三个模型（检测 + 两个坐标）**经本地克隆真实确认缺失**，与先前报告一致；
- 字体与测试视频存在且可用；
- 无项目测试图片；
- 工控机不具备 ONNX 结构审计与坐标模型加载条件，需在 x86 环境补充。
