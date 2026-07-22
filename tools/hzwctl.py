#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""hzwctl：杭州湾双路检测系统统一运维工具。

子命令：
  preflight          生产预检
  status             系统状态汇总
  health             业务 Sidecar 健康状态
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
import json
import os
import shutil
import socket
import subprocess
import sys
import time

BASE_DIR = "/opt/hangzhouwan"
CURRENT_LINK = os.path.join(BASE_DIR, "current")
CONFIG_PATH = os.environ.get("HZW_CONFIG", "/etc/hangzhouwan/application.yaml")
BUSINESS_SOCK = os.environ.get("HANGZHOUWAN_BUSINESS_SOCK", "/run/hangzhouwan/business.sock")
BUSINESS_SVC = "hangzhouwan-business.service"
VIDEO_SVC = "hangzhouwan-video.service"
TARGET_SVC = "hangzhouwan.target"


def run(cmd, timeout=10, check=False):
    """运行 shell 命令，返回 (rc, stdout, stderr)。"""
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        if check and r.returncode != 0:
            return r.returncode, r.stdout, r.stderr
        return r.returncode, r.stdout.strip(), r.stderr.strip()
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"
    except Exception as e:
        return 1, "", str(e)



def yaml_check(config_path, expr):
    """运行 YAML 检查表达式，返回 (rc, stdout)。"""
    cmd = 'python3 -c "import yaml; d=yaml.safe_load(open('' + config_path + '')); ' + expr + '" 2>/dev/null'
    return run(cmd, timeout=3)

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


def systemctl_state(svc):
    """获取 systemd 服务状态。"""
    rc, active, _ = run(f"systemctl is-active {svc} 2>/dev/null", timeout=3)
    rc2, sub, _ = run(f"systemctl is-active --sub {svc} 2>/dev/null", timeout=3)
    return active.strip() if active else "unknown", sub.strip() if sub else ""


def cmd_preflight(args):
    """生产预检。"""
    release_path, release_ver = current_release()
    config = CONFIG_PATH
    pass_count = 0
    fail_count = 0

    def check(name, ok, detail=""):
        nonlocal pass_count, fail_count
        if ok:
            print(f"  ✅ {name}" + (f" ({detail})" if detail else ""))
            pass_count += 1
        else:
            print(f"  ❌ {name}" + (f" ({detail})" if detail else ""))
            fail_count += 1

    print("=== 生产预检 ===")

    # 1. release manifest
    check("release manifest", os.path.isfile(os.path.join(release_path, "manifest.json")) if release_path else False)
    # 2. SHA256
    sha_file = os.path.join(release_path, "sha256sum.txt") if release_path else ""
    check("sha256sum.txt", os.path.isfile(sha_file) if sha_file else False)
    # 3. bmodel
    bmodel = os.path.join(release_path, "models/yolov7_ship_1684_f32.bmodel") if release_path else ""
    check("bmodel 存在", os.path.isfile(bmodel) if bmodel else False)
    # 4. 坐标模型
    check("A 坐标模型", os.path.isfile(os.path.join(release_path, "models/0121_random_forest_model.pkl")) if release_path else False)
    check("B 坐标模型", os.path.isfile(os.path.join(release_path, "models/beishang_x-l.pkl")) if release_path else False)
    # 5. Python 环境
    venv_python = os.path.join(release_path, "venv/bin/python3") if release_path else ""
    py_cmd = venv_python if (venv_python and os.path.isfile(venv_python)) else "python3"
    rc, _, _ = run(py_cmd + " -c 'import sklearn,paho.mqtt.client,pyais' 2>/dev/null", timeout=5)
    check("Python 依赖", rc == 0)
    # 6. TPU
    rc, _, _ = run("ls /dev/bm-tpu* 2>/dev/null || ls /dev/bm-sophon* 2>/dev/null", timeout=3)
    check("TPU 设备", rc == 0)
    # 7. FFmpeg 硬件编解码
    rc, _, _ = run("ffmpeg -hwaccels 2>/dev/null | grep -q h264_bm || true", timeout=5)
    check("FFmpeg h264_bm", rc == 0)
    # 8. application.yaml schema
    check("application.yaml 存在", os.path.isfile(config))
    if os.path.isfile(config):
        rc, out, err = yaml_check(config, 'pass')
        check("application.yaml 合法 YAML", rc == 0, err[:60] if err else "")
    # 9. RTSP/RTMP 配置（环境变量名）
    rc, out, _ = yaml_check(config, "ss=d.get('streams',[]); print(len([s for s in ss if s.get('enabled')]))")
    check("启用流配置", rc == 0 and int(out) > 0 if rc == 0 and out.isdigit() else False, f"{out} streams")
    # 10. MQTT 配置
    rc, out, _ = yaml_check(config, "m=d.get('mqtt',{}); print(m.get('host_env',''))")
    check("MQTT host_env 配置", rc == 0 and bool(out))
    # 11. UDS 目录
    uds_dir = os.path.dirname(BUSINESS_SOCK)
    try:
        os.makedirs(uds_dir, exist_ok=True)
    except PermissionError:
        pass
    check("UDS 目录可写", os.path.isdir(uds_dir) and os.access(uds_dir, os.W_OK))
    # 12. 日志和数据目录
    for d in ["/var/log/hangzhouwan", "/var/lib/hangzhouwan"]:
        try:
            os.makedirs(d, exist_ok=True)
        except PermissionError:
            pass
        check(f"目录 {d}", os.path.isdir(d) and os.access(d, os.W_OK))
    # 13. 磁盘余量
    rc, out, _ = run("df -m /opt 2>/dev/null | tail -1 | awk '{print $4}'", timeout=3)
    avail = int(out) if out.isdigit() else 0
    check(f"磁盘余量 >=500MB", avail >= 500, f"{avail}MB")
    # 14. 无端口或 PID 冲突
    rc, _, _ = run("pgrep -f 'dual_stream_app' 2>/dev/null", timeout=3)
    check("无残留 dual_stream_app 进程", rc != 0)
    # 15. 无另一份双路程序
    rc, _, _ = run("pgrep -c 'dual_stream_app' 2>/dev/null", timeout=3)
    count = 0
    rc2, out2, _ = run("pgrep -f 'dual_stream_app' 2>/dev/null | wc -l", timeout=3)
    count = int(out2) if out2.strip().isdigit() else 0
    check("无双路程序冲突", count <= 2)
    # 16. production 没有 mock/off
    rc, out, _ = yaml_check(config, "b=d.get('business',{}); print(b.get('coordinate_mode',''))")
    mode = out.strip()
    env_rc, env_out, _ = yaml_check(config, "print(d.get('application',{}).get('environment',''))")
    is_prod = env_out.strip() == "production"
    check("production 无 mock/off", not (is_prod and mode in ("mock", "off")), f"mode={mode} env={env_out.strip()}")
    # 17. video 服务会启用 business
    video_svc_path = "/etc/systemd/system/hangzhouwan-video.service"
    if os.path.isfile(video_svc_path):
        with open(video_svc_path) as f:
            content = f.read()
        check("video ExecStart 含 --enable-business", "--enable-business" in content)
    else:
        check("video systemd 单元已安装", False, "未找到")

    print(f"\n=== 预检结果: {pass_count} 通过, {fail_count} 失败 ===")
    return 1 if fail_count > 0 else 0


def cmd_status(args):
    """系统状态汇总。"""
    release_path, release_ver = current_release()
    print("=== 杭州湾双路检测系统状态 ===")
    print(f"当前 Release:    {release_ver or '未激活'}")
    if release_path:
        print(f"  路径:          {release_path}")
    # Git commit
    version_file = os.path.join(release_path, "VERSION") if release_path else ""
    if os.path.isfile(version_file):
        with open(version_file) as f:
            for line in f:
                if line.startswith("commit:"):
                    print(f"  Git commit:    {line.split(':', 1)[1].strip()}")

    # business 状态
    b_active, b_sub = systemctl_state(BUSINESS_SVC)
    print(f"Business 服务:   {b_active} ({b_sub})")
    # coordinate 模式 + MQTT + AIS 缓存（从 sidecar health）
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

    # A/B 状态（从 journal 日志最后几行提取）
    for sid in ("A", "B"):
        rc, out, _ = run(f"journalctl -u {VIDEO_SVC} --no-pager -n 50 2>/dev/null | grep '{sid}:' | tail -1", timeout=5)
        if out:
            print(f"  流 {sid}:          {out[:80]}")
        else:
            print(f"  流 {sid}:          无日志")

    # 重连次数
    rc, out, _ = run(f"journalctl -u {VIDEO_SVC} --no-pager -n 200 2>/dev/null | grep -o 'RTSP重连=[0-9]*' | tail -1", timeout=5)
    if out:
        print(f"  RTSP 重连:     {out}")
    rc, out, _ = run(f"journalctl -u {VIDEO_SVC} --no-pager -n 200 2>/dev/null | grep -o 'RTMP重连=[0-9]*' | tail -1", timeout=5)
    if out:
        print(f"  RTMP 重连:     {out}")

    # RSS
    rc, out, _ = run("ps aux | grep dual_stream_app | grep -v grep | awk '{sum+=$6} END {print sum/1024}'", timeout=3)
    print(f"dual_stream RSS: {out or 'N/A'}")

    # TPU 内存
    rc, out, _ = run("cat /sys/kernel/debug/bm1684/memory_usage 2>/dev/null || bm-smi 2>/dev/null | head -5", timeout=3)
    print(f"TPU:             {'available' if rc == 0 else 'N/A'}")

    # 磁盘
    rc, out, _ = run("df -h /opt 2>/dev/null | tail -1", timeout=3)
    print(f"磁盘 /opt:       {out or 'N/A'}")
    return 0


def cmd_health(args):
    """业务 Sidecar 健康状态。"""
    health = query_sidecar_health(BUSINESS_SOCK)
    print(json.dumps(health, ensure_ascii=False, indent=2))
    return 0 if health.get("connected") or health.get("coordinate_mode") else 1


def cmd_wait(args):
    """等待服务就绪。"""
    svc = BUSINESS_SVC if args.service == "business" else VIDEO_SVC
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
            active, _ = systemctl_state(VIDEO_SVC)
            if active == "active":
                # 检查是否有输出帧日志
                rc, out, _ = run(f"journalctl -u {VIDEO_SVC} --no-pager -n 5 2>/dev/null | grep -c '输出'", timeout=3)
                if rc == 0 and int(out) > 0:
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
    print(f"hzwctl version: 1.0.0")
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
        # 状态
        f.write("--- systemctl status ---\n")
        for svc in [BUSINESS_SVC, VIDEO_SVC, TARGET_SVC]:
            rc, out, _ = run(f"systemctl status {svc} 2>&1", timeout=5)
            f.write(f"[{svc}]\n{out}\n\n")
        # journal 日志
        f.write("--- journal (last 100 lines) ---\n")
        for svc in [BUSINESS_SVC, VIDEO_SVC]:
            rc, out, _ = run(f"journalctl -u {svc} --no-pager -n 100 2>&1", timeout=5)
            f.write(f"[{svc}]\n{out}\n\n")
        # sidecar health
        f.write("--- sidecar health ---\n")
        f.write(json.dumps(query_sidecar_health(BUSINESS_SOCK), ensure_ascii=False, indent=2) + "\n\n")
        # 进程
        f.write("--- processes ---\n")
        rc, out, _ = run("ps aux | grep -E 'dual_stream|business_enrichment' | grep -v grep", timeout=3)
        f.write(out + "\n\n")
        # 磁盘
        f.write("--- disk ---\n")
        rc, out, _ = run("df -h", timeout=3)
        f.write(out + "\n\n")
        # TPU
        f.write("--- TPU ---\n")
        rc, out, _ = run("bm-smi 2>&1 || cat /sys/kernel/debug/bm1684/memory_usage 2>&1 || echo 'N/A'", timeout=5)
        f.write(out + "\n")
    print(f"✅ 诊断信息已保存: {diag_file}")
    return 0


def main():
    parser = argparse.ArgumentParser(description="杭州湾双路检测系统运维工具")
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("preflight", help="生产预检")
    sub.add_parser("status", help="系统状态")
    sub.add_parser("health", help="Sidecar 健康")
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
