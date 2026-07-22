#!/bin/bash
# -*- coding: utf-8 -*-
# 安装 systemd 服务（不自动 enable）。
#
# 用法：
#   sudo bash deploy/install.sh
#
# 安装后需手动 enable：
#   sudo systemctl enable hangzhouwan-business.service
#   sudo systemctl enable hangzhouwan-video.service
#   sudo systemctl start hangzhouwan-business.service
#   # 等待 business readiness
#   sudo systemctl start hangzhouwan-video.service

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVICE_DIR="/etc/systemd/system"
CONFIG_DIR="/data/hangzhouwan/config"

echo "=== 安装杭州湾服务 ==="

# 创建配置目录
sudo mkdir -p "$CONFIG_DIR"
sudo chown "$(id -u):$(id -g)" "$CONFIG_DIR"

# 复制示例配置
if [ ! -f "$CONFIG_DIR/application.yaml" ]; then
    cp "$REPO_ROOT/config/application.example.yaml" "$CONFIG_DIR/application.yaml"
    chmod 600 "$CONFIG_DIR/application.yaml"
    echo "  已创建 $CONFIG_DIR/application.yaml（请编辑填入真实配置）"
fi

# 创建环境变量文件模板
for svc in business video; do
    ENV_FILE="$CONFIG_DIR/${svc}.env"
    if [ ! -f "$ENV_FILE" ]; then
        cat > "$ENV_FILE" <<ENVEOF
# ${svc} 服务环境变量
HZW_ENVIRONMENT=production
COORD_MODE=sklearn
COORD_MODEL_A=$REPO_ROOT/weights/0121_random_forest_model.pkl
COORD_MODEL_B=$REPO_ROOT/weights/beishang_x-l.pkl
AIS_MQTT_HOST=
AIS_MQTT_PORT=1883
AIS_MQTT_CLIENT_ID=
AIS_MQTT_USERNAME=
AIS_MQTT_PASSWORD=
AIS_MQTT_TOPICS=upAIS/base_2250,upAIS/base_2251
HZW_LOG_DIR=$REPO_ROOT/logs/${svc}
ENVEOF
        chmod 600 "$ENV_FILE"
        echo "  已创建 $ENV_FILE（请编辑填入真实凭据）"
    fi
done

# 预检
echo "=== 执行预检 ==="
if ! bash "$REPO_ROOT/deploy/preflight.sh"; then
    echo "❌ 预检未通过，请修复上述问题后重新安装"
    exit 1
fi

# 复制 systemd 单元文件
echo "=== 安装 systemd 单元 ==="
sudo cp "$REPO_ROOT/deploy/systemd/hangzhouwan-business.service" "$SERVICE_DIR/"
sudo cp "$REPO_ROOT/deploy/systemd/hangzhouwan-video.service" "$SERVICE_DIR/"
sudo systemctl daemon-reload

echo ""
echo "=== 安装完成 ==="
echo "服务已安装但未启用。请手动启用："
echo "  sudo systemctl enable hangzhouwan-business.service"
echo "  sudo systemctl enable hangzhouwan-video.service"
echo "  sudo systemctl start hangzhouwan-business.service"
echo "  # 等待 business readiness（health 检查）"
echo "  sudo systemctl start hangzhouwan-video.service"
echo ""
echo "查看状态："
echo "  systemctl status hangzhouwan-business.service"
echo "  systemctl status hangzhouwan-video.service"
