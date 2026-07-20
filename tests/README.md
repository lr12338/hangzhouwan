# 测试框架

本目录存放杭州湾 BM1684 迁移阶段的离线测试。全部基于 Python 标准库 `unittest`，
**无需安装 pytest**，可在工控机当前最小环境（仅 numpy / pyyaml / psutil）直接运行。

## 运行

```bash
# 一次性运行全部测试
python3 tests/run_tests.py

# 仅运行单元测试
python3 -m unittest discover -s tests/unit -v

# 仅运行资产集成测试
python3 -m unittest discover -s tests/integration -v
```

## 目录

| 路径 | 内容 |
|---|---|
| `unit/test_redact_secrets.py` | 脱敏工具单测：RTSP 账号密码、RTMP Key、URL Token、MQTT 密码、普通 URL、空串、非法 URL |
| `unit/test_validate_config.py` | 配置校验单测：YAML 语法、必填字段、环境变量缺失、数值范围、重复 ID、帧率、队列=1、模型缺失、production 拒绝示例值 |
| `integration/test_assets.py` | 资产离线检查：三模型缺失+gitignore、字体存在、测试视频可被 Sophon-FFmpeg 解码、无项目测试图 |
| `fixtures/` | 测试夹具（含示例 manifest） |
| `expected/` | 期望结果 JSON Schema（检测/关联） |

## 约束

- 不连接正式 RTSP / MQTT / RTMP；
- 不安装 SAIL / TPU-MLIR / onnxruntime-gpu / torch；
- 资产检查只判定存在性与可读性，不执行完整船舶推理；
- 测试输出使用中文。
