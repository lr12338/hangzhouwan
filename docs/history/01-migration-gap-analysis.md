# 01 · 源码运行链路复核与迁移差距分析

> 阶段1基于本地完整 Git 克隆逐文件复核原版运行链路。所有结论标注 **文件:行号**。
> 阶段1仅做“不改变算法结果”的安全整改；架构级问题（标注【重写】）留待后续阶段，
> 本阶段不实施。

## 1. 实际运行链路（原版真实流程）

生产入口为 `starter_optimized.py`，使用 `stream_handler_optimized.OptimizedVideoStreamHandler`
+ `utils_demo.config`（路径）+ `config.STREAM_CONFIGS`（流配置）+ `getais`（AIS）：

1. `starter_optimized.py:221` 创建**单一** `YOLOv7` 检测器（ONNX Runtime CUDA）；
2. `starter_optimized.py:240` 调 `initialize_handlers(detector)` 把**同一检测器**传给 A/B 两路；
3. `starter_optimized.py:230`/`238` 每路 `OptimizedVideoStreamHandler` 起三线程：读流 / 处理 / 推流；
4. 读流：`stream_handler_optimized.py:161` `cv2.VideoCapture(stream_url)` 软取流；
5. 处理：`stream_handler_optimized.py` `detect_and_annotate` 调共享检测器 → 逐框坐标预测 → 最近邻 AIS；
6. 推流：`stream_handler_optimized.py` 通过 FFmpeg `stdin` 写 rawvideo，`libx264` 软编码推 RTMP；
7. AIS：`getais.main` 起 MQTT 线程，回调内同步 HTTP 查船名；
8. 监护：`starter_optimized.py:18` `RUN_TIME=3600`，到点 `os.execv` 整进程重启。

## 2. 已确认问题及代码位置

### 2.1 检测与推理

| 问题 | 位置 | 影响 | 处理方案 | 阶段1是否整改 |
|---|---|---|---|---|
| 两路共享一个检测器，`img_width/img_height` 为实例状态 | `detector.py:47`（写入）/ `:148`（读取）；共享于 `starter_optimized.py:221,240` | 两路并发 detect 时实例状态竞态，框坐标缩放错乱 | 每路独立检测器或改用入参传递宽高 | 否（【重写】阶段改 BMRuntime 时消除） |
| 推理后端为 ONNX Runtime CUDA | `detector.py:14-17` `CUDAExecutionProvider` | BM1684 无 CUDA，不可运行 | 改 C++ BMRuntime | 否（【重写】） |
| 输入 640×640 固定 | `detector.py:24-25` | 与模型一致，无问题 | 复用 | - |

### 2.2 视频管线

| 问题 | 位置 | 影响 | 处理方案 | 阶段1是否整改 |
|---|---|---|---|---|
| 软件编码 `libx264` | `stream_handler_optimized.py:100` | 占 CPU、非硬件路径 | 改 `h264_bm` 硬编 | 否（【重写】） |
| 队列容量为 3（非 1） | `stream_handler_optimized.py:32-33` `maxsize=3` | 积压、延迟 | 队列=1，分离推理/输出帧率 | 否（【重写】）；新配置 `runtime.frame_queue_size=1` 已约束 |
| 每帧 `stdin.flush` + `+flush_packets` | `stream_handler_optimized.py:303-304` / `:116` | 每帧 flush 增开销 | 管道消费/重定向，去逐帧 flush | 否（【重写】） |
| FFmpeg stdout/stderr 仅启动失败时读 | `stream_handler_optimized.py:137-138`（PIPE）/ 启动失败 `:146` | 长期运行 stderr 缓冲满可能死锁 | 单独线程消费或重定向 | 否（【重写】） |
| OpenCV 软取流 | `stream_handler_optimized.py:161` `cv2.VideoCapture` | 非 Sophon 硬解 | 改 Sophon-FFmpeg/BMCV 硬解 | 否（【重写】） |
| 三档自适应质量 | `stream_handler_optimized.py:73-87` | 复杂、重启 FFmpeg | 固定参数，非推理帧复用结果 | 否（【重写】） |

### 2.3 坐标映射

| 问题 | 位置 | 影响 | 处理方案 | 阶段1是否整改 |
|---|---|---|---|---|
| 坐标模型**逐检测框** `joblib.load` | `find_ship.py:39,60,82,104,156` | 每框磁盘 I/O + 反序列化，CPU/内存开销大 | 启动时加载一次并缓存 | 否（【重写】）；阶段1仅移除 Windows 路径 |
| 逐框构造 `pd.DataFrame` | `find_ship.py:43,65,87,109,153` | 逐框对象创建开销 | 直接 numpy 输入 | 否（【重写】） |

### 2.4 AIS / MQTT

| 问题 | 位置 | 影响 | 处理方案 | 阶段1是否整改 |
|---|---|---|---|---|
| MQTT 回调内**同步 HTTP** 查船名 | `getais.py:88`（回调内调 `findname`）/ `:150-156`（`requests.get`） | 阻塞 MQTT 回调线程，丢消息 | 异步查询 + 缓存 + 超时 + 重试上限 | 否（【重写】） |
| AIS 异常被静默吞掉（`except: pass`） | `getais.py:89-91` / `:117-118`（整改前） | 故障不可见 | **是**：已改为受控中文日志 `logging.warning/debug` | 是 |
| 全局 AIS 字典被直接访问（绕过锁） | `stream_handler_optimized.py:249` `getais.ship_data_dict`（直读）/ `getais.py:136-139` `get_ais_data()`（带锁拷贝） | 读改竞态 | 统一走带锁快照 | 否（【重写】） |

### 2.5 渲染

| 问题 | 位置 | 影响 | 处理方案 | 阶段1是否整改 |
|---|---|---|---|---|
| 逐框整帧 PIL 转换 | `plot.py:30` `Image.fromarray` | 每框整帧拷贝开销 | OpenCV FreeType 或一次性字体缓存 | 否（【重写】） |
| 逐框加载字体 | `plot.py:32` `ImageFont.truetype` | 每框字体加载开销 | 字体加载一次缓存 | 否（【重写】） |

### 2.6 监护与生命周期

| 问题 | 位置 | 影响 | 处理方案 | 阶段1是否整改 |
|---|---|---|---|---|
| 每小时整进程 `os.execv` 重启 | `starter_optimized.py:18`（`RUN_TIME=3600`）/ `:260` / `:275` / `:136-143` | 全量重启、连接中断 | 去整进程重启，改局部恢复 | 否（【重写】） |
| 无条件终止所有子进程 | `starter_optimized.py:115-127` `children(recursive=True)` + `kill` | 误杀无关子进程 | 精确管理 FFmpeg 句柄 | 否（【重写】） |

### 2.7 配置与安全（阶段1已整改）

| 问题 | 原位置 | 阶段1处理 |
|---|---|---|
| 明文 RTSP 账号密码 / RTMP 推流地址 | `utils_demo/config.py:9,12,26,29` / `config.py:7,10,24,27` / `demo.py:476-485` | 已改为环境变量注入，默认空（禁用） |
| Windows 绝对路径（ffmpeg/字体/模型） | `utils_demo/config.py:41,43,45,48,49` / `find_ship.py:28,49,71,93,140` / `demo.py:220` / `stream_handler_v2.py:63` / `detect.py:171` | 已改为仓库相对路径 + 环境变量覆盖 |
| MQTT 账号密码 / usertoken 硬编码 | `getais.py:24-26,152` / `getais_flask.py:25-27,170` | 已改为环境变量注入 |
| Agora token/APPID/APP证书（注释） | `method.py:6-8` | 已删除注释 |
| EZVIZ 开放令牌（注释/默认） | `config.py:10-11,27-28` / `demo.py:476,482` / `detect.py:172` | 已移除/脱敏 |
| `except: pass` 静默 | `getais.py:89-91,117-118` / `getais_flask.py` 同位置 | 已改为中文受控日志 |

## 3. 复用 / 改造 / 重写 判定（结论）

- **业务知识可复用**：YOLOv7 单类 ship 检测、坐标随机森林映射、AIS 最近邻关联、
  屏蔽区几何判定（`method.py`）、两路 A/B 配置、推流参数取向。
- **代码层面几乎全部重写**（详见阶段0判定）：检测器 → BMRuntime C++；
  视频管线 → Sophon 硬解/硬编；坐标模型 → 启动加载一次；AIS → 异步查询；
  渲染 → OpenCV FreeType；监护 → 去整进程重启。
- **阶段1已完成的安全/配置基础**：凭据外置、Windows 路径移除、中文日志、
  脱敏、配置校验、离线测试、依赖分类，不改变任何算法结果。
