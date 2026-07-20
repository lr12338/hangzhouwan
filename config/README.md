# 配置说明

本目录存放杭州湾双路船舶检测 BM1684 边缘部署的配置文件。

## 文件

| 文件 | 用途 | 是否入库 |
|---|---|---|
| `application.example.yaml` | 应用示例配置（无真实凭据） | 是 |
| `logging.example.yaml` | 日志示例配置 | 是 |
| `application.yaml` | 真实生产配置 | **否**（已 gitignore） |
| `logging.yaml` | 真实日志配置 | **否**（已 gitignore） |

## 敏感值处理原则

1. 源码中不再保留任何明文生产凭据、推流地址或令牌；
2. RTSP / RTMP / MQTT / 船名查询 Token 一律通过环境变量注入；
3. 示例配置中 `*_env` 字段只填写环境变量名，不填写真实值；
4. 真实配置文件部署路径：`/data/hangzhouwan/config/application.yaml`，权限 `chmod 600`；
5. `.env` 文件已加入 `.gitignore`，不得提交仓库。

## 校验

```bash
python3 tools/validate_config.py config/application.example.yaml
```

## 字段速查

- `streams[].enabled`：默认 `false`，未配置凭据时绝不连接正式流；
- `runtime.frame_queue_size` / `processed_queue_size`：必须为 `1`；
- `inference.backend`：正式后端为 `bmrt`（BM1684 BMRuntime）；
- `inference.input_width/height`：640×640（YOLOv7 单类 ship）。
