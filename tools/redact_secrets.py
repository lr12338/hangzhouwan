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

def main():
    import argparse
    parser = argparse.ArgumentParser(description="敏感信息脱敏工具（中文日志）")
    parser.add_argument("text", nargs="?", help="待脱敏的文本；省略则从 stdin 读取")
    args = parser.parse_args()
    src = args.text if args.text is not None else __import__("sys").stdin.read()
    print(redact(src))


if __name__ == "__main__":
    main()
