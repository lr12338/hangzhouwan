#!/bin/bash
# -*- coding: utf-8 -*-
# 卸载 systemd 服务。
#
# 用法：
#   sudo bash deploy/uninstall.sh

set -euo pipefail

SERVICE_DIR="/etc/systemd/system"

echo "=== 卸载杭州湾服务 ==="

# 停止并禁用服务
for svc in hangzhouwan-business hangzhouwan-video; do
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
for svc in hangzhouwan-business hangzhouwan-video; do
    if [ -f "$SERVICE_DIR/$svc.service" ]; then
        sudo rm -f "$SERVICE_DIR/$svc.service"
        echo "  已删除 $SERVICE_DIR/$svc.service"
    fi
done

sudo systemctl daemon-reload

echo "=== 卸载完成 ==="
echo "注意：配置文件 /data/hangzhouwan/config/ 未删除，需手动清理"
