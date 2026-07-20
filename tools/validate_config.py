#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
配置校验工具（中文输出）。

校验 application.yaml：
- YAML 语法；
- 必填字段；
- 环境变量引用（*_env 字段对应的环境变量是否存在，可选强制）；
- 数值范围；
- stream ID 唯一；
- 帧率 / 队列长度合法性；
- production 环境下禁用示例占位值。

仅依赖 Python 标准库 + PyYAML（已随系统提供）。
退出码：0=通过，1=存在错误。
"""
import os
import sys

try:
    import yaml
except ImportError:
    print("错误 | 配置校验 | 未安装 PyYAML，无法解析 YAML 配置")
    sys.exit(1)


class ConfigError(Exception):
    pass


def _require(d, key, ctx):
    if key not in d:
        raise ConfigError(f"{ctx} 缺少必填字段：{key}")
    return d[key]


def _check_int_range(val, lo, hi, ctx):
    if not isinstance(val, int) or isinstance(val, bool):
        raise ConfigError(f"{ctx} 应为整数，实际为 {type(val).__name__}")
    if not (lo <= val <= hi):
        raise ConfigError(f"{ctx} 取值 {val} 越界，允许范围 [{lo}, {hi}]")
    return val


def _check_float_range(val, lo, hi, ctx):
    if isinstance(val, bool) or not isinstance(val, (int, float)):
        raise ConfigError(f"{ctx} 应为数值，实际为 {type(val).__name__}")
    if not (lo <= float(val) <= hi):
        raise ConfigError(f"{ctx} 取值 {val} 越界，允许范围 [{lo}, {hi}]")
    return float(val)


def validate(data, env_strict=False, environ=None):
    """校验已解析的配置字典；返回 (errors, warnings)。"""
    errors = []
    warnings = []
    environ = environ if environ is not None else os.environ
    env = data.get("application", {}).get("environment", "development") if isinstance(data.get("application"), dict) else "development"

    def add_err(msg):
        errors.append(msg)

    try:
        app = _require(data, "application", "application")
        env = _require(app, "environment", "application")
        if env not in ("development", "staging", "production"):
            env = "development"
        if env not in ("development", "staging", "production"):
            add_err(f"application.environment 取值非法：{env}")
        log_level = app.get("log_level", "INFO")
        if log_level not in ("DEBUG", "INFO", "WARNING", "ERROR"):
            add_err(f"application.log_level 取值非法：{log_level}")
    except ConfigError as e:
        add_err(str(e))

    try:
        inf = _require(data, "inference", "inference")
        _require(inf, "backend", "inference")
        _check_int_range(inf.get("input_width", 0), 1, 8192, "inference.input_width")
        _check_int_range(inf.get("input_height", 0), 1, 8192, "inference.input_height")
        _check_float_range(inf.get("confidence_threshold", -1), 0, 1, "inference.confidence_threshold")
        _check_float_range(inf.get("iou_threshold", -1), 0, 1, "inference.iou_threshold")
        # BM1684 生产后端必须为 bmrt
        if env == "production" and inf.get("backend") != "bmrt":
            add_err("production 环境下 inference.backend 必须为 bmrt")
    except ConfigError as e:
        add_err(str(e))

    streams = data.get("streams", [])
    if not isinstance(streams, list) or len(streams) == 0:
        add_err("streams 缺失或为空")
        streams = []
    seen_ids = set()
    for i, s in enumerate(streams):
        ctx = f"streams[{i}]"
        try:
            sid = _require(s, "id", ctx)
            if sid in seen_ids:
                add_err(f"{ctx} stream ID 重复：{sid}")
            seen_ids.add(sid)
            _require(s, "enabled", ctx)
            _require(s, "input_url_env", ctx)
            _require(s, "output_url_env", ctx)
            for fps_field in ("inference_fps", "output_fps"):
                v = s.get(fps_field)
                if v is not None and (not isinstance(v, int) or v < 1 or v > 60):
                    add_err(f"{ctx}.{fps_field} 帧率非法：{v}（允许 1~60）")
            for res_field in ("output_width", "output_height", "output_bitrate_kbps"):
                v = s.get(res_field)
                if v is not None and (not isinstance(v, int) or v <= 0):
                    add_err(f"{ctx}.{res_field} 取值非法：{v}")
            # 环境变量引用校验
            for env_field in ("input_url_env", "output_url_env"):
                var_name = s.get(env_field)
                if var_name and env_strict and not environ.get(var_name):
                    add_err(f"{ctx}.{env_field} 引用的环境变量 {var_name} 未设置")
            # production 环境下必须显式启用且配置坐标模型
            if env == "production":
                if s.get("enabled") is True and not s.get("coordinate_model"):
                    add_err(f"{ctx} production 启用流但未配置 coordinate_model")
                if s.get("enabled") is True and not environ.get(s.get("input_url_env", "")):
                    add_err(f"{ctx} production 启用流但未注入输入环境变量")
        except ConfigError as e:
            add_err(str(e))

    # runtime 队列必须为 1
    rt = data.get("runtime", {})
    for q in ("frame_queue_size", "processed_queue_size"):
        if q in rt:
            try:
                _check_int_range(rt.get(q), 1, 1, f"runtime.{q}")
            except ConfigError as e:
                add_err(f"{e}（队列必须为 1）")

    # 模型路径仅作存在性提示（不阻断校验）
    mp = data.get("inference", {}).get("model_path", "")
    if env == "production" and not mp:
        add_err("production 环境下 inference.model_path 不能为空")
    elif mp and not os.path.exists(mp):
        warnings.append(f"inference.model_path 指向的文件不存在：{mp}")

    return errors, warnings


def load_and_validate(path, env_strict=False, environ=None):
    with open(path, "r", encoding="utf-8") as f:
        try:
            data = yaml.safe_load(f)
        except yaml.YAMLError as e:
            return [f"YAML 语法错误：{e}"], [], None
    if not isinstance(data, dict):
        return ["配置根节点应为映射表"], [], None
    errors, warnings = validate(data, env_strict=env_strict, environ=environ)
    return errors, warnings, data


def main(argv=None):
    import argparse
    parser = argparse.ArgumentParser(description="配置校验工具（中文）")
    parser.add_argument("config", help="application.yaml 路径")
    parser.add_argument("--env-strict", action="store_true", help="强制校验环境变量是否已设置")
    parser.add_argument("--environ-file", help="可选 .env 文件，按 KEY=VALUE 注入校验环境")
    args = parser.parse_args(argv)

    environ = dict(os.environ)
    if args.environ_file and os.path.exists(args.environ_file):
        with open(args.environ_file, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                environ[k.strip()] = v.strip()

    if not os.path.exists(args.config):
        print(f"错误 | 配置校验 | 配置文件不存在：{args.config}")
        return 1

    errors, warnings, data = load_and_validate(args.config, env_strict=args.env_strict, environ=environ)
    for w in warnings:
        print(f"警告 | 配置校验 | {w}")
    if errors:
        for e in errors:
            print(f"错误 | 配置校验 | {e}")
        print(f"结论 | 配置校验 | 未通过，共 {len(errors)} 项错误")
        return 1
    print("信息 | 配置校验 | 配置文件校验通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
