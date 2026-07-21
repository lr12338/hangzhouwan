#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
敏感信息脱敏工具。

面向运维的中文日志脱敏：对 URL、RTSP/RTMP、MQTT 凭据、Authorization 头、
URL 查询 token、环境变量敏感值进行脱敏，仅保留类型与片段，不输出完整凭据。

仅依赖 Python 标准库。
"""
import os
import re
from urllib.parse import urlparse, parse_qs, urlencode, urlunparse

# 敏感环境变量名关键字
SENSITIVE_ENV_KEYS = (
    "PASSWORD", "PASSWD", "TOKEN", "SECRET", "API_KEY", "APIKEY",
    "CREDENTIAL", "PRIVATE_KEY", "STREAM_URL", "OUTPUT_URL", "INPUT_URL",
)


def redact_url(url):
    """脱敏 URL：清除 userinfo、查询参数中的 token/key/password。"""
    if not url or not isinstance(url, str):
        return url
    try:
        parsed = urlparse(url)
    except Exception:
        return "<非法URL>"
    # 清除 userinfo（rtsp://user:pass@host -> rtsp://host）
    netloc = parsed.hostname or ""
    if parsed.port:
        netloc = f"{netloc}:{parsed.port}"
    # 脱敏查询参数
    qs = parse_qs(parsed.query, keep_blank_values=True)
    redacted_qs = {}
    for k, v in qs.items():
        if re.search(r"(token|key|pass|secret|auth|sign|t=)", k, re.IGNORECASE):
            redacted_qs[k] = "<已脱敏>"
        else:
            redacted_qs[k] = v
    new_query = urlencode(redacted_qs, doseq=True)
    return urlunparse((parsed.scheme, netloc, parsed.path, parsed.params, new_query, ""))


def redact_rtmp(url):
    """脱敏 RTMP 推流地址：隐藏路径末尾的流 Key。"""
    if not url or not isinstance(url, str):
        return url
    if not url.startswith("rtmp://"):
        return redact_url(url)
    parsed = urlparse(url)
    netloc = parsed.hostname or ""
    if parsed.port:
        netloc = f"{netloc}:{parsed.port}"
    app = parsed.path.lstrip("/").split("/")
    app_name = app[0] if app else ""
    return f"rtmp://{netloc}/{app_name}/<流Key已脱敏>" if app_name else f"rtmp://{netloc}/<流Key已脱敏>"


def redact_authorization(header_value):
    """脱敏 Authorization 头。"""
    if not header_value or not isinstance(header_value, str):
        return header_value
    if header_value.lower().startswith("bearer "):
        return "Bearer <令牌已脱敏>"
    if header_value.lower().startswith("basic "):
        return "Basic <凭据已脱敏>"
    return "<认证头已脱敏>"


def redact_env_value(key, value):
    """敏感环境变量值脱敏。"""
    if value is None:
        return value
    if any(s in key.upper() for s in SENSITIVE_ENV_KEYS):
        return "<敏感环境变量已脱敏>"
    return value


def redact(text):
    """对任意文本做综合脱敏，返回脱敏后的字符串。"""
    if not text or not isinstance(text, str):
        return text
    out = text
    # 1) rtsp://user:password@host  ->  rtsp://<凭据已脱敏>@host
    out = re.sub(r"(rtsp://)[^@\s]+(@)", r"\1<凭据已脱敏>\2", out)
    # 2) rtmp://host/app/StreamKey  ->  rtmp://host/app/<流Key已脱敏>
    out = re.sub(r"(rtmp://[^\s/]+/[^\s?]+)", lambda m: redact_rtmp(m.group(1)), out)
    # 3) http(s)://...（含查询参数中的 token/key/password）
    out = re.sub(r"(https?://[^\s'\"\)]+)", lambda m: redact_url(m.group(1)), out)
    # 4) 裸键值：usertoken=xxx / token=xxx / password=xxx / api_key=xxx
    out = re.sub(
        r"(?i)(usertoken|token|password|passwd|secret|api_key|apikey)=[^\s&'\"\),}]+",
        r"\1=<已脱敏>", out)
    # 5) Authorization: Bearer xxx / Basic xxx
    out = re.sub(
        r"(?i)(authorization\s*[:：]\s*)(bearer\s+\S+|basic\s+\S+)",
        lambda m: m.group(1) + redact_authorization(m.group(2).strip()), out)
    # 6) 赋值形如  PASSWORD = "xxx"  /  password: xxx
    out = re.sub(r'(?i)(password\s*[:=]\s*)["\']?[^"\'\s,}]+', r'\1<已脱敏>', out)
    return out


# ----- 仓库敏感信息扫描 -----
# 仅检测"看起来是真实凭据"的模式，允许测试/示例文件（按设计含样本凭据）。
_PLACEHOLDERS = (
    "***", "<已脱敏>", "<流Key已脱敏>", "<令牌已脱敏>", "<凭据已脱敏>",
    "<敏感环境变量已脱敏>", "xxx", "xxxx", "example", "your_password",
    "your_token", "<password>", "<secret>", "redacted", "changeme",
)
# 允许含样本凭据的文件（测试/示例/工具自身），扫描时跳过。
_ALLOWLIST_SUFFIXES = (
    "tests/unit/test_redact_secrets.py",
    "tests/unit_cpp/test_rtsp_source_options.cpp",
    "tools/redact_secrets.py",
    ".env.example",
)
_ALLOWLIST_DIRS = ("tests/fixtures/",)

# URL 凭据：scheme://user:password@host （密码非占位符）
_RE_URL_CRED = re.compile(r"(?i)\b(rtmp|rtsp|https?|ftp)://[^:/@\s]+:([^@\s]+)@")
# 赋值凭据：password=xxx / passwd: xxx （值非空且非占位符）
_RE_ASSIGN_CRED = re.compile(r"(?i)(password|passwd|secret|api_?key|token)\s*[:=]\s*['\"]?([^\s'\"#,};]+)")


def _is_placeholder(val):
    if val is None:
        return True
    v = val.strip().strip("'\"").lower()
    if v == "" or v in ("password", "passwd", "secret", "token"):
        return True
    return any(ph in v for ph in _PLACEHOLDERS)


def _looks_like_literal_secret(val):
    """赋值右侧是否像真实硬编码凭据（字面量），而非代码/占位符。
    排除：占位符、空、以及含 ().{} 或以代码关键字开头的值（如 os.environ.get(...)）。"""
    if _is_placeholder(val):
        return False
    v = val.strip().strip("'\"")
    if v == "":
        return False
    # 代码构造（函数调用/属性访问/对象）不是字面量凭据
    if any(c in v for c in "().{}"):
        return False
    if re.match(r"(?i)^(function|os|sys|process|environ|getenv|true|false|null|none|return|var|let|const|this)\b", v):
        return False
    return True


def _line_has_secret(line):
    for m in _RE_URL_CRED.finditer(line):
        if not _is_placeholder(m.group(2)):
            return True
    for m in _RE_ASSIGN_CRED.finditer(line):
        if _looks_like_literal_secret(m.group(2)):
            return True
    return False


def _is_text_file(path):
    try:
        with open(path, "rb") as f:
            chunk = f.read(2048)
    except OSError:
        return False
    if b"\x00" in chunk:
        return False
    return True


def scan_path(root):
    """扫描 root 下文本文件中的真实凭据泄漏。返回 (findings, scanned_count)。
    findings 为 (relpath, lineno, line) 列表。跳过二进制/构建产物/允许列表文件。"""
    skip_dirs = {".git", "build", "artifacts", "__pycache__", "Testing", ".venv", "venv"}
    findings = []
    scanned = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in skip_dirs]
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if any(rel.endswith(suf) for suf in _ALLOWLIST_SUFFIXES):
                continue
            if any(rel.startswith(d) for d in _ALLOWLIST_DIRS):
                continue
            if not _is_text_file(full):
                continue
            scanned += 1
            try:
                with open(full, "r", encoding="utf-8", errors="ignore") as f:
                    for i, line in enumerate(f, 1):
                        if _line_has_secret(line.rstrip("\n")):
                            findings.append((rel, i, line.rstrip("\n")))
            except OSError:
                continue
    return findings, scanned

def main():
    import argparse
    parser = argparse.ArgumentParser(description="敏感信息脱敏与扫描工具（中文日志）")
    parser.add_argument("text", nargs="?", help="待脱敏的文本；省略则从 stdin 读取")
    parser.add_argument("--scan", metavar="PATH", nargs="?", const=".",
                        help="扫描 PATH（默认当前目录）下文本文件的真实凭据泄漏，发现则退出码 1")
    args = parser.parse_args()
    if args.scan is not None:
        findings, scanned = scan_path(args.scan)
        for rel, i, line in findings:
            print(f"{rel}:{i}: {line.strip()}")
        print(f"信息 | 扫描 | 已扫描 {scanned} 个文本文件，发现 {len(findings)} 处疑似凭据泄漏")
        return 1 if findings else 0
    src = args.text if args.text is not None else __import__("sys").stdin.read()
    print(redact(src))


if __name__ == "__main__":
    import sys
    sys.exit(main())
