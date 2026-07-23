#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""hzwctl：杭州湾双路检测系统统一运维工具。

子命令：
  preflight          生产预检（支持 --release/--config/--offline/--activation/--runtime）
  status             系统状态汇总（优先读 Video 健康接口）
  health             健康状态（优先读 Video 健康接口）
  wait-business      等待 business 就绪
  wait-video         等待 video 服务就绪
  start              启动服务（不自动 enable 生产 systemd）
  stop               停止服务
  restart            重启服务
  smoke-test         冒烟测试（300秒内）
  version            版本信息
  collect-diagnostics 收集诊断信息
"""
import argparse
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import time

try:
    import yaml
except ImportError:
    yaml = None

# 确保 sophon 工具（ffmpeg/bm-smi）在 PATH 中（sudo 环境可能不含用户 PATH）
for _sbin in ("/opt/sophon/sophon-ffmpeg-latest/bin",
              "/opt/sophon/sophon-ffmpeg_0.8.0/bin",
              "/opt/sophon/libsophon-current/bin",
              "/opt/sophon/libsophon-0.4.9/bin"):
    if os.path.isdir(_sbin) and _sbin not in os.environ.get("PATH", ""):
        os.environ["PATH"] = _sbin + os.pathsep + os.environ.get("PATH", "")

BASE_DIR = "/opt/hangzhouwan"
CURRENT_LINK = os.path.join(BASE_DIR, "current")
PREVIOUS_LINK = os.path.join(BASE_DIR, "previous")
DEFAULT_CONFIG = os.environ.get("HZW_CONFIG", "/etc/hangzhouwan/application.yaml")
BUSINESS_SOCK = os.environ.get("HANGZHOUWAN_BUSINESS_SOCK", "/run/hangzhouwan/business.sock")
VIDEO_HEALTH_SOCK = os.environ.get("HANGZHOUWAN_VIDEO_HEALTH_SOCK", "/run/hangzhouwan/video-health.sock")
BUSINESS_SVC = "hangzhouwan-business.service"
VIDEO_SVC = "hangzhouwan-video.service"
TARGET_SVC = "hangzhouwan.target"


def run(cmd, timeout=10, check=False):
    """运行 shell 命令，返回 (rc, stdout, stderr)。"""
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout.strip(), r.stderr.strip()
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"
    except Exception as e:
        return 1, "", str(e)


def load_yaml(path):
    """直接 safe_load YAML 文件，返回 dict 或 None。"""
    if not yaml or not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = yaml.safe_load(f)
        return doc if isinstance(doc, dict) else None
    except Exception:
        return None


def sha256_file(path):
    """计算文件 SHA256。"""
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest()
    except Exception:
        return ""


def current_release():
    """获取当前 release 路径和版本。"""
    if os.path.islink(CURRENT_LINK):
        path = os.path.realpath(CURRENT_LINK)
        version_file = os.path.join(path, "VERSION")
        version = ""
        if os.path.isfile(version_file):
            with open(version_file) as f:
                version = f.readline().strip()
        return path, version
    return "", ""


def query_sidecar_health(sock_path, timeout=2):
    """通过 UDS 查询 Sidecar 健康状态。"""
    if not os.path.exists(sock_path):
        return {"error": "socket not found", "connected": False}
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(sock_path)
        s.sendall((json.dumps({"action": "health"}) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        s.close()
        line = buf.split(b"\n")[0].strip()
        if line:
            return json.loads(line.decode())
        return {"error": "empty response", "connected": False}
    except Exception as e:
        return {"error": str(e), "connected": False}


def query_video_health(action="health", timeout=3):
    """通过 Video 健康 Socket 查询结构化指标。

    优先使用此接口，不依赖 journal grep。
    """
    if not os.path.exists(VIDEO_HEALTH_SOCK):
        return {"error": "video-health.sock not found", "available": False}
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(VIDEO_HEALTH_SOCK)
        s.sendall((json.dumps({"action": action}) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        s.close()
        line = buf.split(b"\n")[0].strip()
        if line:
            return json.loads(line.decode())
        return {"error": "empty response", "available": False}
    except Exception as e:
        return {"error": str(e), "available": False}


def systemctl_state(svc):
    """获取 systemd 服务状态。"""
    rc, active, _ = run(f"systemctl is-active {svc} 2>/dev/null", timeout=3)
    rc2, sub, _ = run(f"systemctl is-active --sub {svc} 2>/dev/null", timeout=3)
    return active.strip() if active else "unknown", sub.strip() if sub else ""


# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------

class PreflightResult:
    """预检结果收集器。"""

    def __init__(self):
        self.passes = 0
        self.fails = 0
        self.details = []

    def check(self, name, ok, detail=""):
        if ok:
            print(f"  ✅ {name}" + (f" ({detail})" if detail else ""))
            self.passes += 1
        else:
            print(f"  ❌ {name}" + (f" ({detail})" if detail else ""))
            self.fails += 1
        self.details.append((name, ok, detail))

    @property
    def ok(self):
        return self.fails == 0


def _verify_manifest_shas(release_path, result):
    """校验 manifest.json 内每个文件的 SHA256。"""
    manifest_path = os.path.join(release_path, "manifest.json")
    if not os.path.isfile(manifest_path):
        result.check("manifest.json", False, "缺失")
        return
    try:
        with open(manifest_path) as f:
            manifest = json.load(f)
    except Exception as e:
        result.check("manifest.json 解析", False, str(e))
        return
    result.check("manifest.json 合法", True, f"{len(manifest.get('files', []))} files")

    # 校验 manifest 内每个文件存在且 SHA 匹配（如果 manifest 含 sha 字段）
    files = manifest.get("files", [])
    all_ok = True
    missing = 0
    for rel in files:
        fp = os.path.join(release_path, rel)
        if not os.path.isfile(fp):
            missing += 1
            all_ok = False
            continue
        # 如果 manifest 文件项是 dict 且含 sha256
        if isinstance(rel, dict) and rel.get("sha256"):
            actual = sha256_file(os.path.join(release_path, rel.get("path", "")))
            if actual != rel["sha256"]:
                all_ok = False
    result.check("manifest 文件完整", all_ok and missing == 0,
                 f"{len(files)} files, {missing} missing" if missing else f"{len(files)} files")

    # 校验 bmodel SHA
    bmodel_sha_manifest = manifest.get("bmodel_sha256", "")
    bmodel_path = os.path.join(release_path, "models/yolov7_ship_1684_f32.bmodel")
    if os.path.isfile(bmodel_path):
        actual_sha = sha256_file(bmodel_path)
        if bmodel_sha_manifest:
            result.check("bmodel SHA256 匹配", actual_sha == bmodel_sha_manifest,
                         actual_sha[:12] + "..." if actual_sha else "计算失败")
        else:
            result.check("bmodel SHA256 记录", False, "manifest 未记录 bmodel_sha256")
    else:
        result.check("bmodel 存在", False, f"缺失 {bmodel_path}")

    # 校验坐标模型 SHA（从 sha256sum.txt 或 manifest）
    for model_name, label in (
        ("0121_random_forest_model.pkl", "A 坐标模型 SHA256"),
        ("beishang_x-l.pkl", "B 坐标模型 SHA256"),
    ):
        model_path = os.path.join(release_path, "models", model_name)
        if os.path.isfile(model_path):
            actual_sha = sha256_file(model_path)
            # 检查 sha256sum.txt 是否包含此文件
            sha_file = os.path.join(release_path, "sha256sum.txt")
            if os.path.isfile(sha_file):
                with open(sha_file) as sf:
                    sha_content = sf.read()
                if actual_sha and actual_sha in sha_content:
                    result.check(label, True, actual_sha[:12] + "...")
                else:
                    result.check(label, False, "SHA 不在 sha256sum.txt")
            else:
                result.check(label, True, actual_sha[:12] + "..." if actual_sha else "计算失败")
        else:
            result.check(label, False, f"缺失 {model_name}")

    # 校验 sha256sum.txt（如果存在）
    sha_file = os.path.join(release_path, "sha256sum.txt")
    if os.path.isfile(sha_file):
        rc, out, err = run(
            f"cd '{release_path}' && grep -v './venv/' sha256sum.txt | sha256sum -c --quiet 2>&1",
            timeout=30)
        result.check("sha256sum.txt 校验", rc == 0,
                     "全部通过" if rc == 0 else (err or out)[:60])
    else:
        result.check("sha256sum.txt", False, "缺失")


def _check_ffmpeg(result):
    """真实检查 FFmpeg 硬件解码器和编码器。"""
    # 解码器
    rc, out, _ = run("ffmpeg -decoders 2>/dev/null | grep -w 'h264_bm'", timeout=5)
    result.check("FFmpeg h264_bm 解码器", rc == 0 and "h264_bm" in out,
                 "h264_bm" if rc == 0 and "h264_bm" in out else "未找到")
    # 编码器
    rc, out, _ = run("ffmpeg -encoders 2>/dev/null | grep -w 'h264_bm'", timeout=5)
    result.check("FFmpeg h264_bm 编码器", rc == 0 and "h264_bm" in out,
                 "h264_bm" if rc == 0 and "h264_bm" in out else "未找到")
    # hwaccels（诊断信息）
    rc, out, _ = run("ffmpeg -hwaccels 2>/dev/null", timeout=5)
    result.check("FFmpeg hwaccels 可用", rc == 0, out.replace("\n", ",")[:60] if out else "")


def _check_config_consistency(config_path, result):
    """检查 Sidecar（Python）和 Video（C++）配置一致性。"""
    doc = load_yaml(config_path)
    if not doc:
        result.check("application.yaml 解析", False, "无法加载")
        return
    result.check("application.yaml 解析", True, "safe_load 成功")

    # 环境
    env = doc.get("application", {}).get("environment", "development")
    result.check("environment 合法", env in ("development", "staging", "production"), env)

    # production 禁止 mock/off
    biz = doc.get("business", {})
    mode = biz.get("coordinate_mode", "sklearn")
    if env == "production" and mode in ("mock", "off"):
        result.check("production 无 mock/off", False, f"mode={mode}")
    else:
        result.check("production 无 mock/off", True, f"mode={mode} env={env}")

    # 启用流
    streams = doc.get("streams", [])
    enabled = [s for s in streams if s.get("enabled")]
    result.check("启用流配置", len(enabled) > 0, f"{len(enabled)} streams")

    # MQTT 配置
    mqtt = doc.get("mqtt", {})
    result.check("MQTT host_env 配置", bool(mqtt.get("host_env")), mqtt.get("host_env", ""))
    result.check("MQTT topics 配置", bool(mqtt.get("topics")), f"{len(mqtt.get('topics', []))} topics")

    # socket 路径一致性：business.socket_path 必须与 video 使用的 socket 一致
    socket_path = biz.get("socket_path", "/run/hangzhouwan/business.sock")
    result.check("business socket_path", socket_path == "/run/hangzhouwan/business.sock", socket_path)

    # 坐标模型路径：business 的 model_a_path/model_b_path 必须与 streams 的 coordinate_model 一致
    model_a = biz.get("model_a_path", "")
    model_b = biz.get("model_b_path", "")
    stream_a_model = ""
    stream_b_model = ""
    for s in streams:
        if s.get("id") == "A":
            stream_a_model = s.get("coordinate_model", "")
        if s.get("id") == "B":
            stream_b_model = s.get("coordinate_model", "")
    # 检查路径一致（如果都配置了）
    if model_a and stream_a_model:
        result.check("A 坐标模型路径一致", os.path.basename(model_a) == os.path.basename(stream_a_model),
                     f"biz={os.path.basename(model_a)} stream={os.path.basename(stream_a_model)}")
    else:
        result.check("A 坐标模型路径", bool(model_a), model_a or "未配置")
    if model_b and stream_b_model:
        result.check("B 坐标模型路径一致", os.path.basename(model_b) == os.path.basename(stream_b_model),
                     f"biz={os.path.basename(model_b)} stream={os.path.basename(stream_b_model)}")
    else:
        result.check("B 坐标模型路径", bool(model_b), model_b or "未配置")

    # MQTT 主题一致：mqtt.topics 与 ais.topics（如果存在）
    ais = doc.get("ais", {})
    if ais.get("topics") and mqtt.get("topics"):
        result.check("MQTT/AIS 主题一致",
                     set(ais["topics"]) == set(mqtt["topics"]),
                     f"ais={ais['topics']} mqtt={mqtt['topics']}")

    # 灰度输出与 Windows 正式推流地址冲突检查
    _check_grayscale_conflict(doc, result)


def _check_release_files(release_path, result):
    """检查 release 目录关键文件。"""
    result.check("VERSION 存在", os.path.isfile(os.path.join(release_path, "VERSION")))
    result.check("manifest.json 存在", os.path.isfile(os.path.join(release_path, "manifest.json")))

    # bmodel
    bmodel = os.path.join(release_path, "models/yolov7_ship_1684_f32.bmodel")
    result.check("bmodel 存在", os.path.isfile(bmodel))

    # 坐标模型
    result.check("A 坐标模型",
                 os.path.isfile(os.path.join(release_path, "models/0121_random_forest_model.pkl")))
    result.check("B 坐标模型",
                 os.path.isfile(os.path.join(release_path, "models/beishang_x-l.pkl")))

    # Python 环境
    venv_python = os.path.join(release_path, "venv/bin/python3")
    py_cmd = venv_python if os.path.isfile(venv_python) else "python3"
    rc, _, _ = run(f"{py_cmd} -c 'import sklearn,paho.mqtt.client,pyais' 2>/dev/null", timeout=10)
    result.check("Python 依赖 (sklearn/paho/pyais)", rc == 0)

    # dual_stream_app 可执行
    app_path = os.path.join(release_path, "bin/dual_stream_app")
    result.check("dual_stream_app 可执行", os.path.isfile(app_path) and os.access(app_path, os.X_OK))

    # hzwctl 可执行
    hzwctl_path = os.path.join(release_path, "bin/hzwctl")
    result.check("hzwctl 可执行", os.path.isfile(hzwctl_path) and os.access(hzwctl_path, os.X_OK))


def _load_env_files():
    """从 /etc/hangzhouwan/{video,business}.env 加载 KEY=VALUE（跳过注释）。"""
    env = {}
    for name in ("video.env", "business.env"):
        path = os.path.join("/etc/hangzhouwan", name)
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    k, _, v = line.partition("=")
                    env[k.strip()] = v.strip()
        except Exception:
            pass
    return env


def _resolve_env_value(env_name, env_files):
    """优先进程环境，其次 .env 文件。"""
    val = os.environ.get(env_name)
    if val:
        return val
    return env_files.get(env_name, "")


def _check_dynamic_libs(release_path, result):
    """检查 dual_stream_app 动态库依赖，存在 not found 则预检失败。"""
    app = os.path.join(release_path, "bin/dual_stream_app")
    if not os.path.isfile(app):
        return
    rc, out, _ = run(f"ldd '{app}' 2>&1", timeout=10)
    not_found = [ln.strip() for ln in out.splitlines() if "not found" in ln]
    result.check("动态库无 not found", not not_found,
                 "; ".join(not_found[:3]) if not_found else "ldd OK")


def _check_grayscale_conflict(doc, result):
    """灰度输出不得与 Windows 正式推流地址冲突。

    application.yaml 的 deploy.forbidden_formal_outputs 列出正式输出地址；
    每路启用流的 output_url（由 output_url_env 解析）不得命中其中任一。
    """
    deploy = doc.get("deploy", {}) or {}
    forbidden = deploy.get("forbidden_formal_outputs", []) or []
    if not forbidden:
        result.check("灰度/正式输出冲突检查", True, "未配置 forbidden_formal_outputs（跳过）")
        return
    env_files = _load_env_files()
    streams = doc.get("streams", []) or []
    conflict = None
    checked = 0
    for st in streams:
        if not st.get("enabled"):
            continue
        out_env = st.get("output_url_env", "")
        if not out_env:
            continue
        out_url = _resolve_env_value(out_env, env_files)
        if not out_url:
            continue
        checked += 1
        for furl in forbidden:
            if furl and (out_url == furl or out_url.rstrip("/") == furl.rstrip("/")):
                conflict = (st.get("id", "?"), out_url)
                break
        if conflict:
            break
    if conflict:
        result.check("灰度/正式输出不冲突", False, f"流 {conflict[0]} 命中正式地址")
    elif checked == 0:
        result.check("灰度/正式输出不冲突", True, "未解析到输出地址（环境变量未注入）")
    else:
        result.check("灰度/正式输出不冲突", True, f"{checked} 路灰度地址均未冲突")


def cmd_preflight(args):
    """生产预检。

    支持模式：
      hzwctl preflight                                  # 检查 current release
      hzwctl preflight --release <candidate>            # 检查候选 release（不读 current）
      hzwctl preflight --release <candidate> --config <path>
      hzwctl preflight --offline                        # 仅静态检查
      hzwctl preflight --activation                     # 仅激活条件检查
      hzwctl preflight --runtime                        # 仅运行时冲突检查
    """
    # 确定 release 路径：候选 > current
    if args.release:
        release_path = os.path.realpath(args.release)
        if not os.path.isdir(release_path):
            print(f"错误: 候选 Release 目录不存在: {release_path}")
            return 1
    else:
        release_path, _ = current_release()
        if not release_path:
            print("错误: current Release 未激活，请使用 --release <candidate>")
            return 1

    config_path = args.config or DEFAULT_CONFIG

    # 确定运行哪些阶段
    any_phase = args.offline or args.activation or args.runtime
    do_offline = not any_phase or args.offline
    do_activation = not any_phase or args.activation
    do_runtime = not any_phase or args.runtime

    result = PreflightResult()
    print("=== 生产预检 ===")
    print(f"  Release: {release_path}")
    print(f"  Config:  {config_path}")
    print(f"  模式:    {'/'.join(p for p, f in (('offline', do_offline), ('activation', do_activation), ('runtime', do_runtime)) if f)}")
    print()

    if do_offline:
        print("--- 离线静态检查 ---")
        _check_release_files(release_path, result)
        _verify_manifest_shas(release_path, result)
        _check_dynamic_libs(release_path, result)
        _check_ffmpeg(result)
        _check_config_consistency(config_path, result)

        # TPU 设备
        rc, _, _ = run("ls /dev/bm-tpu* 2>/dev/null || ls /dev/bm-sophon* 2>/dev/null", timeout=3)
        result.check("TPU 设备", rc == 0)

        # UDS 目录
        uds_dir = os.path.dirname(BUSINESS_SOCK)
        try:
            os.makedirs(uds_dir, exist_ok=True)
        except PermissionError:
            pass
        result.check("UDS 目录可写", os.path.isdir(uds_dir) and os.access(uds_dir, os.W_OK))
        print()

    if do_activation:
        print("--- 激活条件检查 ---")
        # current 是否存在
        result.check("current 链接状态",
                     os.path.islink(CURRENT_LINK) or not os.path.exists(CURRENT_LINK),
                     "已存在" if os.path.islink(CURRENT_LINK) else "不存在（首次激活）")

        # 磁盘余量
        rc, out, _ = run("df -m /opt 2>/dev/null | tail -1 | awk '{print $4}'", timeout=3)
        avail = int(out) if out.isdigit() else 0
        result.check("磁盘余量 >=500MB", avail >= 500, f"{avail}MB")

        # 日志和数据目录
        for d in ["/var/log/hangzhouwan", "/var/lib/hangzhouwan"]:
            try:
                os.makedirs(d, exist_ok=True)
            except PermissionError:
                pass
            result.check(f"目录 {d}", os.path.isdir(d) and os.access(d, os.W_OK))

        # video systemd 单元
        video_svc_path = "/etc/systemd/system/hangzhouwan-video.service"
        if os.path.isfile(video_svc_path):
            with open(video_svc_path) as f:
                content = f.read()
            result.check("video ExecStart 含 --enable-business", "--enable-business" in content)
            result.check("video ExecStartPre 含 wait-business", "wait-business" in content)
            result.check("video 无 RuntimeDirectory", "RuntimeDirectory" not in content)
        else:
            # 回退到 release 内的 systemd 单元
            svc_in_release = os.path.join(release_path, "systemd/hangzhouwan-video.service")
            if os.path.isfile(svc_in_release):
                with open(svc_in_release) as f:
                    content = f.read()
                result.check("video ExecStart 含 --enable-business", "--enable-business" in content)
                result.check("video ExecStartPre 含 wait-business", "wait-business" in content)
                result.check("video 无 RuntimeDirectory", "RuntimeDirectory" not in content)
            else:
                result.check("video systemd 单元", False, "未找到")

        # business systemd 单元
        biz_svc_path = "/etc/systemd/system/hangzhouwan-business.service"
        check_path = biz_svc_path if os.path.isfile(biz_svc_path) else \
            os.path.join(release_path, "systemd/hangzhouwan-business.service")
        if os.path.isfile(check_path):
            with open(check_path) as f:
                content = f.read()
            result.check("business ExecStart 含 --config", "--config" in content)
            result.check("business 无 RuntimeDirectory", "RuntimeDirectory" not in content)
        else:
            result.check("business systemd 单元", False, "未找到")

        # tmpfiles.d 统一管理共享运行目录 /run/hangzhouwan
        tmpfiles_installed = "/etc/tmpfiles.d/hangzhouwan.conf"
        tmpfiles_release = os.path.join(release_path, "tmpfiles.d/hangzhouwan.conf")
        tmpfiles_path = tmpfiles_installed if os.path.isfile(tmpfiles_installed) else tmpfiles_release
        if os.path.isfile(tmpfiles_path):
            try:
                with open(tmpfiles_path) as f:
                    tf = f.read()
            except Exception:
                tf = ""
            result.check("tmpfiles.d 管理 /run/hangzhouwan", "/run/hangzhouwan" in tf, tmpfiles_path)
        else:
            result.check("tmpfiles.d 管理 /run/hangzhouwan", False, "缺失")
        print()

    if do_runtime:
        print("--- 运行时冲突检查 ---")
        # 残留进程：直接扫描 /proc，排除自身进程树避免自匹配
        my_pid = os.getpid()
        my_ppid = os.getppid()
        residual_app = []
        residual_biz = []
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            pid = int(entry)
            if pid in (my_pid, my_ppid):
                continue
            try:
                with open(f"/proc/{pid}/cmdline", "rb") as cf:
                    cmdline = cf.read().replace(b"\x00", b" ").decode("utf-8", "replace")
            except (IOError, OSError):
                continue
            if "dual_stream_app" in cmdline and "pgrep" not in cmdline and "hzwctl" not in cmdline:
                residual_app.append(pid)
            if "business_enrichment.app" in cmdline and "pgrep" not in cmdline and "hzwctl" not in cmdline:
                residual_biz.append(pid)
        result.check("无残留 dual_stream_app", not residual_app,
                     f"PID={','.join(map(str, residual_app))}" if residual_app else "")
        result.check("无残留 business 进程", not residual_biz,
                     f"PID={','.join(map(str, residual_biz))}" if residual_biz else "")

        # systemd 重启风暴检查
        for svc in [BUSINESS_SVC, VIDEO_SVC]:
            rc, out, _ = run(f"systemctl show {svc} -p NRestarts --value 2>/dev/null", timeout=3)
            restarts = int(out) if out.isdigit() else 0
            result.check(f"{svc} 无重启风暴", restarts < 10, f"NRestarts={restarts}")

        # 端口冲突（RTMP 默认 1935）
        rc, out, _ = run("ss -tlnp 2>/dev/null | grep ':1935' | grep -v dual_stream", timeout=3)
        result.check("无 RTMP 端口冲突", rc != 0, out[:60] if rc == 0 else "")
        print()

    print(f"=== 预检结果: {result.passes} 通过, {result.fails} 失败 ===")
    return 1 if result.fails > 0 else 0


def cmd_status(args):
    """系统状态汇总（优先读 Video 健康接口）。"""
    release_path, release_ver = current_release()
    print("=== 杭州湾双路检测系统状态 ===")
    print(f"当前 Release:    {release_ver or '未激活'}")
    if release_path:
        print(f"  路径:          {release_path}")
    version_file = os.path.join(release_path, "VERSION") if release_path else ""
    if os.path.isfile(version_file):
        with open(version_file) as f:
            for line in f:
                if line.startswith("commit:"):
                    print(f"  Git commit:    {line.split(':', 1)[1].strip()}")

    # business 状态
    b_active, b_sub = systemctl_state(BUSINESS_SVC)
    print(f"Business 服务:   {b_active} ({b_sub})")
    health = query_sidecar_health(BUSINESS_SOCK)
    if health.get("connected", False) or "coordinate_mode" in health:
        print(f"  coordinate:    {health.get('coordinate_mode', '?')}")
        print(f"  MQTT:          {'connected' if health.get('mqtt_connected') else 'disconnected'}")
        print(f"  AIS 缓存:      {health.get('ais_cache_count', '?')}")
    else:
        print(f"  Sidecar:       {health.get('error', 'unreachable')}")

    # video 状态
    v_active, v_sub = systemctl_state(VIDEO_SVC)
    print(f"Video 服务:      {v_active} ({v_sub})")

    # 优先从 Video 健康 Socket 读取
    vh = query_video_health("health")
    if vh.get("available", False) and "error" not in vh:
        status = vh.get("status", "UNKNOWN")
        # 状态头条：HEALTHY/DEGRADED/FAILED 醒目展示，避免"降级状态 none"假健康
        marker = {"HEALTHY": "OK", "DEGRADED": "DEGRADED", "FAILED": "FAILED"}.get(status, "UNKNOWN")
        print("  (数据来源: Video 健康 Socket)")
        print(f"  状态:          [{marker}] {status}")
        reason = vh.get("health_reason", "")
        if reason:
            print(f"  原因:          {reason}")
        print(f"  release:       {vh.get('release', '?')}")
        print(f"  commit:        {vh.get('commit', '?')}")
        for sid in ("A", "B"):
            s = vh.get("streams", {}).get(sid, {})
            if s:
                lvl = s.get("level", "UNKNOWN")
                print(f"  流 {sid}:          [{lvl}]")
                print(f"    RTSP:         {'connected' if s.get('rtsp_connected') else 'disconnected'}")
                print(f"    RTMP:         {'connected' if s.get('rtmp_connected') else 'disconnected'}")
                print(f"    output_fps:   {s.get('output_fps', '?')}")
                print(f"    inference_fps:{s.get('inference_fps', '?')}")
                print(f"    RTSP重连:     {s.get('rtsp_reconnects', '?')}")
                print(f"    RTMP重连:     {s.get('rtmp_reconnects', '?')}")
                print(f"    最近帧(ms):   {s.get('last_frame_time_ms', '?')}")
                print(f"    队列长度:     {s.get('queue_length', '?')}")
                print(f"    e2e P95:      {s.get('e2e_p95_ms', '?')}ms")
        print(f"  Business:      {vh.get('business_state', '?')}")
        print(f"  降级状态:      {vh.get('degradation', 'none')}")
        print(f"  uptime:        {vh.get('uptime_seconds', '?')}s")
        print(f"  RSS:           {vh.get('rss_mb', '?')}MB")
        print(f"  TPU:           {vh.get('tpu_info', '?')}")
    else:
        # 回退到 journal grep（诊断补充）
        print(f"  (Video Socket 不可用: {vh.get('error', 'N/A')}，回退 journal)")
        for sid in ("A", "B"):
            rc, out, _ = run(f"journalctl -u {VIDEO_SVC} --no-pager -n 50 2>/dev/null | grep '{sid}:' | tail -1", timeout=5)
            if out:
                print(f"  流 {sid}:          {out[:80]}")
            else:
                print(f"  流 {sid}:          无日志")

    # 磁盘
    rc, out, _ = run("df -h /opt 2>/dev/null | tail -1", timeout=3)
    print(f"磁盘 /opt:       {out or 'N/A'}")
    return 0


def cmd_health(args):
    """健康状态（优先读 Video 健康接口，回退 Sidecar）。"""
    vh = query_video_health("health")
    if vh.get("available", False) and "error" not in vh:
        status = vh.get("status", "UNKNOWN")
        print("=== Video 健康（Socket）===")
        print(json.dumps(vh, ensure_ascii=False, indent=2))
        return {"HEALTHY": 0, "DEGRADED": 1, "FAILED": 2}.get(status, 2)

    print("=== Business Sidecar 健康 ===")
    health = query_sidecar_health(BUSINESS_SOCK)
    print(json.dumps(health, ensure_ascii=False, indent=2))
    return 0 if health.get("connected") or health.get("coordinate_mode") else 1


def cmd_wait(args):
    """等待服务就绪。"""
    timeout = args.timeout
    deadline = time.time() + timeout
    if args.service == "business":
        print(f"等待 business 就绪（最长 {timeout}s）...")
        while time.time() < deadline:
            h = query_sidecar_health(BUSINESS_SOCK)
            if h.get("coordinate_mode") or h.get("connected"):
                print("✅ business 就绪")
                return 0
            time.sleep(1)
        print("❌ business 超时未就绪")
        return 1
    else:
        print(f"等待 video 就绪（最长 {timeout}s）...")
        while time.time() < deadline:
            # 优先检查 Video 健康 Socket
            vh = query_video_health("health", timeout=1)
            if vh.get("available", False) and "error" not in vh:
                print("✅ video 就绪（健康 Socket 可用）")
                return 0
            active, _ = systemctl_state(VIDEO_SVC)
            if active == "active":
                rc, out, _ = run(f"journalctl -u {VIDEO_SVC} --no-pager -n 5 2>/dev/null | grep -c '输出'", timeout=3)
                if rc == 0 and out.isdigit() and int(out) > 0:
                    print("✅ video 就绪")
                    return 0
            time.sleep(1)
        print("❌ video 超时未就绪")
        return 1


def cmd_start(args):
    """启动服务（不自动 enable）。"""
    print("启动 hangzhouwan.target（不自动 enable）...")
    rc, out, err = run(f"sudo systemctl start {TARGET_SVC}", timeout=30)
    if rc != 0:
        print(f"启动失败: {err}")
        return 1
    print("✅ 已启动（需手动 enable 才能开机自启）")
    return 0


def cmd_stop(args):
    """停止服务。"""
    rc, _, err = run(f"sudo systemctl stop {TARGET_SVC}", timeout=30)
    print("✅ 已停止" if rc == 0 else f"停止失败: {err}")
    return rc


def cmd_restart(args):
    """重启服务。"""
    rc, _, err = run(f"sudo systemctl restart {TARGET_SVC}", timeout=30)
    print("✅ 已重启" if rc == 0 else f"重启失败: {err}")
    return rc


def cmd_smoke_test(args):
    """冒烟测试（300秒内）。"""
    print("=== 冒烟测试（最长 300s）===")
    checks = [
        ("business 健康", lambda: query_sidecar_health(BUSINESS_SOCK).get("coordinate_mode") is not None),
        ("video active", lambda: systemctl_state(VIDEO_SVC)[0] == "active"),
        ("video 健康 Socket", lambda: query_video_health("health").get("available", False)),
    ]
    pass_c = 0
    fail_c = 0
    for name, fn in checks:
        try:
            ok = fn()
        except Exception:
            ok = False
        if ok:
            print(f"  ✅ {name}")
            pass_c += 1
        else:
            print(f"  ❌ {name}")
            fail_c += 1
    print(f"\n=== 冒烟测试: {pass_c} 通过, {fail_c} 失败 ===")
    return 1 if fail_c > 0 else 0


def cmd_version(args):
    """版本信息。"""
    release_path, release_ver = current_release()
    print(f"hzwctl version: 2.0.0")
    print(f"当前 Release:  {release_ver or '未激活'}")
    if release_path and os.path.isfile(os.path.join(release_path, "VERSION")):
        with open(os.path.join(release_path, "VERSION")) as f:
            print(f.read())
    return 0


def cmd_collect_diagnostics(args):
    """收集诊断信息。"""
    diag_dir = os.environ.get("HZW_DIAG_DIR", "/var/log/hangzhouwan/diagnostics")
    os.makedirs(diag_dir, exist_ok=True)
    ts = time.strftime("%Y%m%d_%H%M%S")
    diag_file = os.path.join(diag_dir, f"diag_{ts}.txt")
    print(f"收集诊断信息到 {diag_file}...")
    with open(diag_file, "w") as f:
        f.write(f"=== 诊断报告 {ts} ===\n\n")
        f.write("--- systemctl status ---\n")
        for svc in [BUSINESS_SVC, VIDEO_SVC, TARGET_SVC]:
            rc, out, _ = run(f"systemctl status {svc} 2>&1", timeout=5)
            f.write(f"[{svc}]\n{out}\n\n")
        f.write("--- journal (last 100 lines) ---\n")
        for svc in [BUSINESS_SVC, VIDEO_SVC]:
            rc, out, _ = run(f"journalctl -u {svc} --no-pager -n 100 2>&1", timeout=5)
            f.write(f"[{svc}]\n{out}\n\n")
        f.write("--- sidecar health ---\n")
        f.write(json.dumps(query_sidecar_health(BUSINESS_SOCK), ensure_ascii=False, indent=2) + "\n\n")
        f.write("--- video health socket ---\n")
        f.write(json.dumps(query_video_health("health"), ensure_ascii=False, indent=2) + "\n\n")
        f.write("--- processes ---\n")
        rc, out, _ = run("ps aux | grep -E 'dual_stream|business_enrichment' | grep -v grep", timeout=3)
        f.write(out + "\n\n")
        f.write("--- disk ---\n")
        rc, out, _ = run("df -h", timeout=3)
        f.write(out + "\n\n")
        f.write("--- TPU ---\n")
        rc, out, _ = run("bm-smi 2>&1 || cat /sys/kernel/debug/bm1684/memory_usage 2>&1 || echo 'N/A'", timeout=5)
        f.write(out + "\n")
    print(f"✅ 诊断信息已保存: {diag_file}")
    return 0


def main():
    parser = argparse.ArgumentParser(description="杭州湾双路检测系统运维工具")
    sub = parser.add_subparsers(dest="command")

    p_pre = sub.add_parser("preflight", help="生产预检")
    p_pre.add_argument("--release", type=str, default="", help="候选 Release 目录（不读 current）")
    p_pre.add_argument("--config", type=str, default="", help="配置文件路径")
    p_pre.add_argument("--offline", action="store_true", help="仅静态检查")
    p_pre.add_argument("--activation", action="store_true", help="仅激活条件检查")
    p_pre.add_argument("--runtime", action="store_true", help="仅运行时冲突检查")

    sub.add_parser("status", help="系统状态")
    sub.add_parser("health", help="健康状态")
    w = sub.add_parser("wait-business", help="等待 business 就绪")
    w.add_argument("--timeout", type=int, default=30)
    w2 = sub.add_parser("wait-video", help="等待 video 就绪")
    w2.add_argument("--timeout", type=int, default=60)
    sub.add_parser("start", help="启动服务")
    sub.add_parser("stop", help="停止服务")
    sub.add_parser("restart", help="重启服务")
    sub.add_parser("smoke-test", help="冒烟测试")
    sub.add_parser("version", help="版本")
    sub.add_parser("collect-diagnostics", help="收集诊断")

    args = parser.parse_args()
    if not args.command:
        parser.print_help()
        return 1

    commands = {
        "preflight": (cmd_preflight, args),
        "status": (cmd_status, args),
        "health": (cmd_health, args),
        "wait-business": (cmd_wait, argparse.Namespace(service="business", timeout=getattr(args, "timeout", 30))),
        "wait-video": (cmd_wait, argparse.Namespace(service="video", timeout=getattr(args, "timeout", 60))),
        "start": (cmd_start, args),
        "stop": (cmd_stop, args),
        "restart": (cmd_restart, args),
        "smoke-test": (cmd_smoke_test, args),
        "version": (cmd_version, args),
        "collect-diagnostics": (cmd_collect_diagnostics, args),
    }
    fn, fn_args = commands[args.command]
    return fn(fn_args)


if __name__ == "__main__":
    sys.exit(main())
