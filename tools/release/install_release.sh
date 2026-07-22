#!/bin/bash
# -*- coding: utf-8 -*-
# 安装 Release 到系统（systemd 单元 + 配置目录），不自动 enable/start。
#
# 用法：
#   sudo bash tools/release/install_release.sh <release_dir>
#   sudo bash tools/release/install_release.sh /opt/hangzhouwan/releases/20260722-abc123

set -euo pipefail

RELEASE_DIR="${1:-/opt/hangzhouwan/current}"
if [ ! -d "$RELEASE_DIR" ]; then
  echo "错误: Release 目录不存在: $RELEASE_DIR"
  exit 1
fi
RELEASE_DIR="$(cd "$RELEASE_DIR" && pwd)"

CONFIG_DIR="/etc/hangzhouwan"
SERVICE_DIR="/etc/systemd/system"

echo "=== 安装 Release: ${RELEASE_DIR} ==="

# 1. 创建配置目录
mkdir -p "$CONFIG_DIR"
chown "$(id -u):$(id -g)" "$CONFIG_DIR" 2>/dev/null || true

# 2. 安装配置（不覆盖已有 application.yaml）
if [ ! -f "$CONFIG_DIR/application.yaml" ]; then
  cp "$RELEASE_DIR/config/application.example.yaml" "$CONFIG_DIR/application.yaml"
  chmod 600 "$CONFIG_DIR/application.yaml"
  echo "  已创建 $CONFIG_DIR/application.yaml（请编辑填入真实配置）"
else
  echo "  $CONFIG_DIR/application.yaml 已存在，保留"
fi

# 3. 创建环境变量文件模板（不覆盖）
for svc in business video; do
  ENV_FILE="$CONFIG_DIR/${svc}.env"
  if [ ! -f "$ENV_FILE" ]; then
    cat > "$ENV_FILE" <<ENVEOF
# ${svc} 服务环境变量（权限 600）
HZW_ENVIRONMENT=production
COORD_MODE=sklearn
ENVEOF
    chmod 600 "$ENV_FILE"
    echo "  已创建 $ENV_FILE（请编辑填入真实凭据）"
  fi
done

# 4. 安装 systemd 单元
if [ -d "$RELEASE_DIR/systemd" ]; then
  cp "$RELEASE_DIR"/systemd/*.service "$SERVICE_DIR/" 2>/dev/null || true
  cp "$RELEASE_DIR"/systemd/*.target "$SERVICE_DIR/" 2>/dev/null || true
  systemctl daemon-reload
  echo "  systemd 单元已安装（未 enable）"
fi

# 5. 运行预检
if [ -x "$RELEASE_DIR/bin/hzwctl" ]; then
  echo "=== 执行预检 ==="
  "$RELEASE_DIR/bin/hzwctl" preflight || {
    echo "⚠️  预检未完全通过，请修复后 activate"
  }
fi

echo ""
echo "=== 安装完成 ==="
echo "激活: bash tools/release/activate_release.sh $RELEASE_DIR"
echo "启动: sudo systemctl start hangzhouwan.target"
