#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Classify strict health separately from degraded data-plane availability."""

HEALTHY = "HEALTHY"
OPERATIONAL_DEGRADED = "OPERATIONAL_DEGRADED"
UNAVAILABLE = "UNAVAILABLE"


def _business_healthy(health):
    return bool(
        health
        and health.get("status") == "HEALTHY"
        and health.get("model_a_loaded")
        and health.get("model_b_loaded")
    )


def classify_availability(video, business, config):
    """Return (classification, details) from structured health snapshots."""
    health_cfg = config.get("health", {})
    healthy_output = float(health_cfg.get("stream_healthy_fps", 9))
    degraded_output = float(health_cfg.get("stream_degraded_fps", 5))
    healthy_inference = float(health_cfg.get("inference_healthy_fps", 4))
    enabled = [
        str(stream.get("id"))
        for stream in config.get("streams", [])
        if stream.get("enabled") and stream.get("id")
    ] or ["A", "B"]
    streams = (video or {}).get("streams", {})
    storage_ok = (video or {}).get("storage", {}).get("state") in (
        "OK", "WARNING")
    resource_fatal = bool(
        (video or {}).get("resource_fatal")
        or (video or {}).get("resource", {}).get("fatal")
    )
    business_ok = _business_healthy(business)
    strict_streams = []
    operational_streams = []
    for stream_id in enabled:
        stream = streams.get(stream_id, {})
        connected = bool(
            stream.get("rtsp_connected") and stream.get("rtmp_connected"))
        running = stream.get("lifecycle", "RUNNING") == "RUNNING"
        output_fps = float(stream.get("output_fps", 0) or 0)
        inference_fps = float(stream.get("inference_fps", 0) or 0)
        if (running and connected and output_fps >= degraded_output
                and inference_fps > 0):
            operational_streams.append(stream_id)
        if (running and connected and output_fps >= healthy_output
                and inference_fps >= healthy_inference):
            strict_streams.append(stream_id)

    business_link_ok = (video or {}).get("business_link") in (
        "HEALTHY", "DISABLED")
    strict = bool(
        video
        and video.get("status") == "HEALTHY"
        and storage_ok
        and not resource_fatal
        and len(strict_streams) == len(enabled)
        and business_link_ok
        and (business_ok or video.get("business_link") == "DISABLED")
    )
    details = {
        "video_status": (video or {}).get("status", "UNAVAILABLE"),
        "storage_ok": storage_ok,
        "resource_fatal": resource_fatal,
        "business_healthy": business_ok,
        "enabled_streams": enabled,
        "strict_streams": strict_streams,
        "operational_streams": operational_streams,
    }
    if strict:
        return HEALTHY, details
    if video and storage_ok and not resource_fatal and operational_streams:
        return OPERATIONAL_DEGRADED, details
    return UNAVAILABLE, details
