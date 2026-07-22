# 迁移进度跟踪（PROGRESS）

> 本文件是**当前测试进展的唯一权威索引**。每完成一个阶段或关键决策，更新本文件。新开发者先读 [../README.md](../README.md)，再读本文件，再按 [00-index.md](00-index.md) 深入。

| 项 | 值 |
|---|---|
| 最后更新 | 2026-07-22（阶段7收口：systemd/hzwctl/release修复、Video健康接口、配置一致性、91项Python测试全通过） |
| 仓库 | `https://github.com/lr12338/hangzhouwan.git` |
| 本地路径 | `/home/linaro/hangzhouwan-orign/hangzhouwan` |
| 当前分支 | `feat/bm1684-edge-deployment` |
| HEAD | `a9a10dc`（工作区有本次修改未提交） |
| 当前阶段（阶段7收口） | **代码完成、实装待验证**：systemd配置修复、hzwctl preflight重写、release激活+自动回滚、Video健康Socket、配置一致性、PROGRESS修正 |
| 总体健康度 | 🟡 阶段7代码完成，T1-T10短测+L1-L4长测待人工在BM1684真实环境执行；不自动enable生产systemd、不关闭Windows |

---

## 阶段7：生产实装收口（2026-07-22，代码完成、实装待验证）

### 本次修复项

| # | 内容 | 状态 | 关键文件 |
|---|------|------|----------|
| 1 | Business ExecStart 添加 --config /etc/hangzhouwan/application.yaml | ✅ | `deploy/systemd/hangzhouwan-business.service` |
| 2 | Socket 路径统一 /run/hangzhouwan/business.sock（Python+C+++YAML） | ✅ | `services/business_enrichment/config.py`, `config/application.example.yaml` |
| 3 | Python config 从 YAML env var 名称解析 MQTT host（与C++一致） | ✅ | `services/business_enrichment/config.py` |
| 4 | Video ExecStartPre=hzwctl wait-business --timeout 30 | ✅ | `deploy/systemd/hangzhouwan-video.service` |
| 5 | Video 添加 network-online.target 依赖 | ✅ | `deploy/systemd/hangzhouwan-video.service` |
| 6 | RuntimeDirectory 只由 Business 声明，Video 不声明 | ✅ | `deploy/systemd/hangzhouwan-video.service` |
| 7 | hzwctl preflight 重写（safe_load替代shell拼接） | ✅ | `tools/hzwctl.py` |
| 8 | hzwctl preflight FFmpeg 解码器/编码器分别真实检查（删除|| true） | ✅ | `tools/hzwctl.py` |
| 9 | hzwctl preflight --release/--config/--offline/--activation/--runtime | ✅ | `tools/hzwctl.py` |
| 10 | hzwctl preflight manifest SHA 逐文件校验 + bmodel/坐标模型SHA | ✅ | `tools/hzwctl.py` |
| 11 | hzwctl preflight 配置一致性检查（socket/模型/MQTT主题） | ✅ | `tools/hzwctl.py` |
| 12 | activate_release.sh 完整流程：verify→preflight→smoke→mv -T→restart→wait→60s smoke→自动回滚 | ✅ | `tools/release/activate_release.sh` |
| 13 | Video 健康 Socket /run/hangzhouwan/video-health.sock（health/metrics/version） | ✅ | `include/monitoring/video_health_server.h`, `src/monitoring/video_health_server.cpp` |
| 14 | DualStreamApplication 集成健康 Socket（metrics_loop更新状态） | ✅ | `src/application/dual_stream_application.cpp` |
| 15 | SingleStreamPipeline 暴露 business_state() | ✅ | `include/pipeline/single_stream_pipeline.h` |
| 16 | hzwctl status/health 优先读 Video 健康 Socket | ✅ | `tools/hzwctl.py` |
| 17 | build_release.sh 安装 hzwctl Python 脚本为 bin/hzwctl | ✅ | `tools/release/build_release.sh` |
| 18 | CMakeLists.txt 添加 video_health_server.cpp | ✅ | `CMakeLists.txt` |
| 19 | 91项 Python 单元测试全通过（含49项新增） | ✅ | `tests/unit/test_systemd_config.py`, `test_business_config.py`, `test_preflight.py` |

### 测试结果

| 测试 | 结果 |
|---|---|
| Python 单元测试（91项） | ✅ 全部通过 |
| systemd 配置测试（15项） | ✅ Business --config, Video ExecStartPre, RuntimeDirectory归属 |
| BusinessConfig 测试（10项） | ✅ YAML覆盖, env只覆盖敏感字段, production禁止mock/off, socket一致 |
| preflight 源码测试（8项） | ✅ 无shell拼接, 无|| true, safe_load, --release/--config/--offline/--activation/--runtime |
| preflight 候选Release测试（6项） | ✅ 不读current, manifest SHA校验, 配置一致性 |
| activate_release 测试（10项） | ✅ verify/preflight/smoke/回滚/mv -T/wait-business/wait-video/无git/无编译/结果码 |
| C++ 编译 | ⏳ 待在BM1684目标板编译（含新增 video_health_server.cpp） |
| T1-T10 短测 | ⏳ 待人工在BM1684真实环境执行 |

### 生产候选门禁

| 门禁 | 状态 |
|------|------|
| systemd 实际启用业务融合 | ✅ ExecStart 含 --enable-business |
| Business 从 application.yaml 读取配置 | ✅ ExecStart 含 --config |
| application.yaml 为唯一权威配置 | ✅ C++/Python 共同读取 |
| Socket 路径一致 | ✅ /run/hangzhouwan/business.sock |
| Video readiness 依赖 Business | ✅ ExecStartPre=wait-business |
| RuntimeDirectory 仅 Business | ✅ Video 不声明 |
| preflight 不用 shell 拼接 YAML | ✅ yaml.safe_load |
| preflight FFmpeg 真实检查 | ✅ 解码器/编码器分别检查 |
| preflight 支持候选Release | ✅ --release/--config |
| preflight SHA 校验 | ✅ manifest逐文件 + bmodel SHA |
| release 激活含自动回滚 | ✅ 失败切回previous |
| Video 结构化健康接口 | ✅ /run/hangzhouwan/video-health.sock |
| hzwctl 优先读健康接口 | ✅ 不依赖 journal grep |
| 300 秒灰度通过 | ⏳ 待人工（T10，需真实流） |
| 30 分钟通过 | ⏳ 待人工（L1） |
| 2 小时通过 | ⏳ 待人工（L2） |
| 8 小时通过 | ⏳ 待人工（L3） |
| 24 小时通过 | ⏳ 待人工（L4） |
| A 路符合目标 | ⏳ 待人工验证 |
| B 路达到批准阈值 | ⏳ 待人工验证 |
| 真实 MQTT 消息验证 | ⏳ 待人工（需有船经过） |
| 真实 AIS 人工样本验证 | ⏳ 待人工（A/B 各 ≥20 样本） |
| Windows 回切演练成功 | ⏳ 待人工 |

### 红线遵守

- ✅ 不自动 enable 正式 systemd（install 不 enable）
- ✅ 不关闭 Windows 旧服务
- ✅ 不执行超过 300 秒的自动测试
- ✅ 不使用 git checkout 作为生产回滚
- ✅ production 禁止 mock/off 坐标
- ✅ video 服务 ExecStart 含 --enable-business
- ✅ video 服务 ExecStartPre 含 wait-business
- ✅ RuntimeDirectory 仅 Business 拥有
- ✅ 灰度输出不覆盖 Windows 正式输出
- ✅ 未提交真实凭据/日志/视频

---

## 1. 阶段总览

| # | 阶段 | 状态 | 门禁 | 备注 |
|---|---|---|---|---|
| 0 | 项目与设备基线确认 | ✅ 完成 | 通过 | 三份文档 + chip 探针已交付 |
| 1 | 测试基线 + 安全配置 | ✅ 完成 | 源码可复现、离线测试通过 | 26 项测试 |
| 2 | ONNX 审计 + bmodel 转换 | ✅ 完成 | bmodel 可加载推理 | f32 转换完成 |
| 3 | 单图 C++ 推理 PoC | ✅ 完成 | 检测结果正确 | C++ + BMRuntime |
| 4 | 单路视频硬件管线 | ✅ 完成 | 5min 冒烟通过 | h264_bm 解码+编码 |
| 4.2 | 单路 RTSP 输入 | ✅ 完成 | 连接路径验证 | 硬件链路未回退 |
| 4.3 | 实流 RTMP 推流 | ✅ 完成 | 双路并发稳定 | A/B 独立管线 |
| 5 | 双路全栈+坐标+AIS | ✅ 完成 | 端到端验证 | 坐标真实模型+MQTT AIS |
| 6 | 生产收口（配置驱动+release+hzwctl） | ✅ 完成 | 17项预检+T0-T8+F1-F12 | 生产候选架构 |
| 7 | 生产实装收口（systemd/hzwctl/release/健康接口修复） | ✅ 代码完成 | 91项Python测试通过 | **实装待验证** |
| 8 | 人工长测（30min/2h/8h/24h） | ⏳ 待人工 | 真实流环境 | L1-L4 |

---

## 2. 阶段5：双路全栈 + 坐标预测 + MQTT AIS 关联

详见 `docs/25-dual-stream-full-stack.md`、`docs/27-coordinate-ais-mqtt-architecture.md`。

- A/B 双路并发运行，各自独立 RTSP/RTMP/推理/编码线程
- 坐标预测 Sidecar（Python）：sklearn 随机森林 + numpy forest 两种后端
- MQTT AIS 订阅 + 视觉-AIS 匹配（距离+时间对齐+外推）
- Business Sidecar UDS 协议 v2（JSON 分隔符协议）
- 真实 MQTT AIS 端到端验证通过

## 3. 阶段6：生产收口

- C++ application_config YAML 模块 + schema 校验
- dual_stream_app 配置驱动（删除硬编码）
- EnrichedDetection 融合快照类型 + 彩色绘制
- 合法 JSONL + AIS 证据修复
- systemd 配置驱动 + readiness
- Release 制品 + 原子升降级
- hzwctl 运维工具 + 17 项生产预检
- T0-T8 短测 + F1-F12 故障测试

## 4. 阶段7：生产实装收口（本次）

### 4.1 systemd 修复
- Business ExecStart 添加 `--config /etc/hangzhouwan/application.yaml`
- Video 添加 `ExecStartPre=hzwctl wait-business --timeout 30`
- Video 添加 `network-online.target` 依赖
- RuntimeDirectory 仅由 Business 声明（restart video 不删除 business.sock）
- readiness（ExecStartPre）与 runtime degradation（C++ BusinessEnrichmentClient 自动降级/恢复）分开处理

### 4.2 hzwctl preflight 重写
- YAML 直接 `yaml.safe_load`（不再 shell 拼接 `python3 -c`）
- FFmpeg 解码器和编码器分别真实检查（删除 `|| true` 假通过）
- 支持 `--release <candidate> --config <path>`（候选预检不读 current）
- 运行冲突检查拆分：`--offline` / `--activation` / `--runtime`
- manifest 逐文件 SHA 校验 + bmodel SHA + 坐标模型 SHA
- Sidecar 和 Video 配置一致性检查（socket/模型/MQTT主题）

### 4.3 Release 激活改进
- 完整流程：verify → preflight → 离线smoke → mv -T原子替换 → restart → wait-business → wait-video → 60s smoke → 自动回滚
- 失败自动切回 previous，重启 previous，验证 readiness
- 禁止现场编译和 git checkout
- 明确结果码（0=成功, 2=verify失败, 3=preflight失败, 4=smoke失败, 5=business未就绪, 6=video未就绪, 7=60s smoke失败）

### 4.4 Video 结构化健康接口
- Unix Socket: `/run/hangzhouwan/video-health.sock`
- 支持: health / metrics / version
- 返回: release/version/commit, A/B RTSP/RTMP状态, A/B output/inference fps, A/B最近帧时间, A/B RTSP/RTMP重连次数, 队列长度, e2e P95, Business连接状态, 降级状态, RSS和TPU信息
- hzwctl status/health 优先读取此接口，journal 只作为诊断补充

### 4.5 配置一致性
- Socket 路径统一: `/run/hangzhouwan/business.sock`（Python config.py + C++ ApplicationConfig + example YAML）
- Python config 从 YAML env var 名称解析 MQTT host（与 C++ resolve_env 一致）
- 模型路径、MQTT 主题在 business 和 streams/ais 段一致

---

## 5. 阶段8：人工长测方案（待人工执行）

### 短测（每项最长300秒）

| 测试 | 内容 | 预期 |
|------|------|------|
| T1 | 候选Release静态预检 | preflight --release --offline 通过 |
| T2 | systemd-analyze verify | 无语法错误 |
| T3 | Business从YAML启动并readiness通过 | wait-business 成功 |
| T4 | Video等待Business后启动 | wait-video 成功 |
| T5 | Video健康Socket | health/metrics/version 返回真实指标 |
| T6 | restart video不影响Business Socket | business.sock 存在 |
| T7 | restart business时Video降级并恢复 | DETECTION_ONLY → FULL |
| T8 | 候选Release激活成功 | current指向新Release |
| T9 | 错误候选Release自动回滚 | current切回previous |
| T10 | 灰度双路300秒 | A/B推流可见，不覆盖Windows |

### 长测

| 测试 | 时长 | 门禁 |
|------|------|------|
| L1 | 30分钟 | 无崩溃，fps稳定，无内存泄漏 |
| L2 | 2小时 | 无崩溃，RTSP/RTMP稳定，Business融合正常 |
| L3 | 8小时 | 无崩溃，无重启风暴，磁盘不满 |
| L4 | 24小时 | 全部门禁通过，可批准替换Windows |

### 真实数据门禁

| 门禁 | 要求 |
|------|------|
| 真实MQTT消息 | MQTT连接订阅通过，有真实AIS消息 |
| 真实AIS样本 | A/B各≥20样本，坐标非0，匹配率达标 |
| Windows回切 | 停止systemd服务后Windows可正常接管 |
