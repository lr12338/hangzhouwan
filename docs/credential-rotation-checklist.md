# 凭据轮换清单（Credential Rotation Checklist）

> 仓库 `https://github.com/lr12338/hangzhouwan` 为 **public**。原版源码与初始提交
> `56d380f3` 中包含明文生产凭据，且 80MB 运行时产物 `output.txt` 亦嵌入了完整
> RTSP/RTMP 地址。**所有下列凭据视为已泄露，必须在进入生产前全部轮换。**
>
> 本清单只记录凭据类型、原位置、是否已从“当前源码”移除；**不记录任何完整凭据值**。

## 1. 必须轮换的凭据（已泄露于公开 git 历史）

| # | 凭据类型 | 原位置（文件:行，整改前） | 当前源码是否已移除 | 轮换动作 |
|---|---|---|---|---|
| 1 | 摄像机 RTSP 账号密码 | `utils_demo/config.py:9,26` / `config.py:7,24` / `demo.py:478,484`(注释) | 是 | 修改摄像机 Web 后台密码；新密码仅经环境变量 `STREAM_A_INPUT_URL`/`STREAM_B_INPUT_URL` 注入 |
| 2 | RTMP 推流地址（含流 Key） | `utils_demo/config.py:12,29` / `config.py:10,27` / `demo.py:479,485` | 是 | 更换推流 App/StreamKey；新值仅经 `STREAM_A_OUTPUT_URL`/`STREAM_B_OUTPUT_URL` 注入 |
| 3 | EZVIZ(萤石) HLS 开放令牌 | `utils_demo/config.py:10-11,27-28`(注释) / `demo.py:476,482` / `detect.py:172` | 是 | 在萤石开放平台作废旧令牌并重发；不再硬编码 |
| 4 | MQTT 账号/密码 | `utils_demo/getais.py:24-26` / `getais_flask.py:25-27` | 是 | 更换 MQTT 用户/密码；新值经 `AIS_MQTT_CLIENT_ID/USERNAME/PASSWORD` 注入 |
| 5 | 船名查询 HTTP 端点 + usertoken | `utils_demo/getais.py:152` / `getais_flask.py:170` | 是 | 轮换 token；端点与令牌均外置，经 `SHIP_NAME_API_ENDPOINT`/`SHIP_NAME_API_TOKEN` 注入，默认空则不调用 |
| 6 | 声网(Agora) token / APPID / APP证书 | `utils_demo/method.py:6-8`(注释) | 是 | 在 Agora 控制台重置 APP 证书；如不再使用则注销 |
| 7 | 嵌入运行时产物的 RTSP/RTMP 地址 | `hangzhouwan_beishang/output.txt`（80MB，已 untrack） | 是（已 untrack） | 同 #1/#2 一并轮换；产物不再入库 |

## 2. 历史泄露处理

- 初始提交 `56d380f3` 与 `output.txt` 的 blob 仍保留在 git 历史中。
- 按任务约束 **本阶段不得重写 git 历史**，故不执行 `git filter-repo` / BFG 清史。
- 处理策略：**以轮换凭据替代清史**（凭据已公开，清史无法消除已发生的泄露）。
- 若后续需要清理历史，须在 x86 开发机单独操作并强制推送（需用户授权，本阶段不做）。

## 3. 轮换后的注入方式

凭据不再写入源码或示例配置，统一通过环境变量注入（见 `.env.example`）：

| 环境变量 | 用途 |
|---|---|
| `STREAM_A_INPUT_URL` / `STREAM_B_INPUT_URL` | A/B 路 RTSP 输入 |
| `STREAM_A_OUTPUT_URL` / `STREAM_B_OUTPUT_URL` | A/B 路 RTMP 推流 |
| `AIS_MQTT_CLIENT_ID` / `AIS_MQTT_USERNAME` / `AIS_MQTT_PASSWORD` | MQTT 凭据 |
| `SHIP_NAME_API_ENDPOINT` | 船名查询服务端点（默认空则不调用） |
| `SHIP_NAME_API_TOKEN` | 船名查询令牌 |

真实部署：`/data/hangzhouwan/config/.env`（权限 `chmod 600`），不入库。

## 4. 整改后自检

- 对已知生产凭据特征串（摄像机口令、推流主机、MQTT 账号、船名令牌、EZVIZ 令牌等，
  具体值见内部轮换记录，不在本文件展开）执行 `git grep`，在当前分支源码
  （`*.py/*.md/*.bat/*.txt/*.yaml`）中 **0 命中**（测试夹具已改用合成值，不含真实凭据）。
- 示例配置 `config/application.example.yaml` 无任何真实地址/用户名/密码/令牌。
- 脱敏工具 `tools/redact_secrets.py` 与单测 `tests/unit/test_redact_secrets.py` 覆盖
  RTSP 账号密码、RTMP Key、URL Token、MQTT 密码、Authorization、普通 URL、空串、非法 URL。
