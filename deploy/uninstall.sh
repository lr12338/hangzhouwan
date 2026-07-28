#!/bin/bash
# -*- coding: utf-8 -*-
# 卸载 systemd 服务。
#
# 用法：
#   sudo bash deploy/uninstall.sh

set -euo pipefail

SERVICE_DIR="/etc/systemd/system"

echo "=== 卸载杭州湾服务 ==="

# 停止并禁用定时维护
if systemctl is-enabled hangzhouwan-maintenance-restart.timer >/dev/null 2>&1; then
    sudo systemctl disable --now hangzhouwan-maintenance-restart.timer
    echo "  已禁用定时维护"
fi

# 停止并禁用服务
for svc in hangzhouwan-supervisor hangzhouwan-video hangzhouwan-business; do
    if systemctl is-enabled "$svc.service" >/dev/null 2>&1; then
        sudo systemctl disable "$svc.service"
        echo "  已禁用 $svc"
    fi
    if systemctl is-active "$svc.service" >/dev/null 2>&1; then
        sudo systemctl stop "$svc.service"
        echo "  已停止 $svc"
    fi
done

# 删除单元文件
for unit in \
    hangzhouwan-business.service \
    hangzhouwan-video.service \
    hangzhouwan-supervisor.service \
    hangzhouwan-maintenance-restart.service \
    hangzhouwan-maintenance-restart.timer \
    hangzhouwan.target; do
    if [ -f "$SERVICE_DIR/$unit" ]; then
        sudo rm -f "$SERVICE_DIR/$unit"
        echo "  已删除 $SERVICE_DIR/$unit"
    fi
done

for file in \
    /etc/udev/rules.d/70-hangzhouwan-devices.rules \
    /etc/systemd/journald.conf.d/50-hangzhouwan-limits.conf \
    /etc/rsyslog.d/30-hangzhouwan-journal-only.conf \
    /etc/tmpfiles.d/hangzhouwan.conf; do
    if [ -f "$file" ]; then
        sudo rm -f "$file"
        echo "  已删除 $file"
    fi
done

sudo systemctl daemon-reload
sudo udevadm control --reload-rules 2>/dev/null || true

echo "=== 卸载完成 ==="
echo "注意：/etc/hangzhouwan 与 /data/hangzhouwan 未删除，需按数据保留策略处理"
