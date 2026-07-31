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

# 0. Release 完整性和所有权是安装硬门禁。
if ! (cd "$RELEASE_DIR" && sha256sum -c sha256sum.txt --quiet); then
  echo "错误: Release SHA256 校验失败" >&2
  exit 2
fi
if [ "$(stat -c '%U:%G' "$RELEASE_DIR")" != "root:root" ]; then
  echo "错误: Release 必须由 root:root 拥有" >&2
  exit 2
fi
if find "$RELEASE_DIR" \( ! -user root -o ! -group root \) -print -quit |
   grep -q .; then
  echo "错误: Release 内存在非 root:root 文件" >&2
  exit 2
fi
if find "$RELEASE_DIR" \( -type f -o -type d \) -perm /022 \
   -print -quit | grep -q .; then
  echo "错误: Release 内存在组/其他用户可写路径" >&2
  exit 2
fi

# 1. 独立服务账户、共享运行组和目录权限。
getent group hangzhouwan >/dev/null || groupadd --system hangzhouwan
for account in hangzhouwan-video hangzhouwan-business; do
  if ! id "$account" >/dev/null 2>&1; then
    useradd --system --no-create-home --home-dir /nonexistent \
      --shell /usr/sbin/nologin --gid hangzhouwan "$account"
  fi
done
install -d -o root -g hangzhouwan -m 0750 "$CONFIG_DIR"
chown root:hangzhouwan /data/hangzhouwan
chmod 0751 /data/hangzhouwan
install -d -o hangzhouwan-video -g hangzhouwan -m 0770 \
  /data/hangzhouwan/events
install -d -o root -g hangzhouwan -m 0770 \
  /data/hangzhouwan/monitor /run/hangzhouwan
find /data/hangzhouwan/events -maxdepth 1 -type f \
  -name 'stream_*.current.jsonl' -exec \
  chown hangzhouwan-video:hangzhouwan {} +
find /data/hangzhouwan/events -maxdepth 1 -type f \
  -name 'stream_*.current.jsonl' -exec chmod 0640 {} +

# 2. 安装配置（不覆盖已有 application.yaml）
if [ ! -f "$CONFIG_DIR/application.yaml" ]; then
  cp "$RELEASE_DIR/config/application.example.yaml" "$CONFIG_DIR/application.yaml"
  chown root:hangzhouwan "$CONFIG_DIR/application.yaml"
  chmod 640 "$CONFIG_DIR/application.yaml"
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
chown root:hangzhouwan "$CONFIG_DIR/application.yaml" "$CONFIG_DIR"/*.env
chmod 640 "$CONFIG_DIR/application.yaml" "$CONFIG_DIR"/*.env

# 4. 安装 systemd 单元
if [ -d "$RELEASE_DIR/systemd" ]; then
  cp "$RELEASE_DIR"/systemd/*.service "$SERVICE_DIR/" 2>/dev/null || true
  cp "$RELEASE_DIR"/systemd/*.timer "$SERVICE_DIR/" 2>/dev/null || true
  cp "$RELEASE_DIR"/systemd/*.target "$SERVICE_DIR/" 2>/dev/null || true
  chown root:root "$SERVICE_DIR"/hangzhouwan*.service \
    "$SERVICE_DIR"/hangzhouwan*.timer "$SERVICE_DIR"/hangzhouwan*.target \
    2>/dev/null || true
  chmod 644 "$SERVICE_DIR"/hangzhouwan*.service \
    "$SERVICE_DIR"/hangzhouwan*.timer "$SERVICE_DIR"/hangzhouwan*.target \
    2>/dev/null || true
  systemd-analyze verify "$SERVICE_DIR"/hangzhouwan*.service \
    "$SERVICE_DIR"/hangzhouwan*.timer "$SERVICE_DIR"/hangzhouwan*.target
  systemctl daemon-reload
  for unit_path in "$RELEASE_DIR"/systemd/*.service \
                   "$RELEASE_DIR"/systemd/*.timer \
                   "$RELEASE_DIR"/systemd/*.target; do
    [ -e "$unit_path" ] || continue
    unit_name="$(basename "$unit_path")"
    if ! cmp -s "$unit_path" "$SERVICE_DIR/$unit_name"; then
      echo "错误: systemd 单元复制后不一致: $unit_name" >&2
      exit 3
    fi
    if [ "$(systemctl show "$unit_name" -p LoadState --value)" != "loaded" ]; then
      echo "错误: systemd 单元未加载: $unit_name" >&2
      exit 3
    fi
  done
  target_wants="$(systemctl show hangzhouwan.target -p Wants --value)"
  case " $target_wants " in
    *" hangzhouwan-maintenance-recovery.timer "*) ;;
    *)
      echo "错误: hangzhouwan.target 未加载恢复 timer 依赖" >&2
      exit 3
      ;;
  esac
  echo "  systemd 单元已安装（未 enable）"
fi

# 7. 设备白名单及系统日志策略。
if [ -f "$RELEASE_DIR/udev/70-hangzhouwan-devices.rules" ]; then
  install -o root -g root -m 0644 \
    "$RELEASE_DIR/udev/70-hangzhouwan-devices.rules" \
    /etc/udev/rules.d/70-hangzhouwan-devices.rules
  udevadm control --reload-rules
  udevadm trigger --subsystem-match=char --action=change 2>/dev/null || true
  for device in /dev/bm-tpu0 /dev/bm-vpp /dev/bmdev-ctl /dev/ion /dev/jpu /dev/vpu; do
    if [ -e "$device" ]; then chgrp hangzhouwan "$device"; chmod 0660 "$device"; fi
  done
fi
if [ -f "$RELEASE_DIR/journald/50-hangzhouwan-limits.conf" ]; then
  install -D -o root -g root -m 0644 \
    "$RELEASE_DIR/journald/50-hangzhouwan-limits.conf" \
    /etc/systemd/journald.conf.d/50-hangzhouwan-limits.conf
fi
if [ -f "$RELEASE_DIR/rsyslog/30-hangzhouwan-journal-only.conf" ]; then
  install -o root -g root -m 0644 \
    "$RELEASE_DIR/rsyslog/30-hangzhouwan-journal-only.conf" \
    /etc/rsyslog.d/30-hangzhouwan-journal-only.conf
fi
if [ -f "$RELEASE_DIR/logrotate/rsyslog" ]; then
  install -o root -g root -m 0644 "$RELEASE_DIR/logrotate/rsyslog" \
    /etc/logrotate.d/rsyslog
fi

# 6. 安装 tmpfiles.d（共享运行目录 /run/hangzhouwan）
TMPFILES_SRC="$RELEASE_DIR/tmpfiles.d/hangzhouwan.conf"
if [ ! -f "$TMPFILES_SRC" ]; then
  TMPFILES_SRC="$(dirname "$RELEASE_DIR")/../deploy/tmpfiles.d/hangzhouwan.conf"
fi
if [ -f "$TMPFILES_SRC" ]; then
  install -o root -g root -m 0644 \
    "$TMPFILES_SRC" /etc/tmpfiles.d/hangzhouwan.conf
  systemd-tmpfiles --create /etc/tmpfiles.d/hangzhouwan.conf 2>/dev/null || true
  echo "  tmpfiles.d 已安装（/run/hangzhouwan 已创建）"
fi

# 5. 运行预检
if [ -x "$RELEASE_DIR/bin/hzwctl" ]; then
  echo "=== 执行预检 ==="
  "$RELEASE_DIR/bin/hzwctl" preflight --release "$RELEASE_DIR" --config /etc/hangzhouwan/application.yaml --offline || {
    echo "⚠️  预检未完全通过，请修复后 activate"
  }
fi

echo ""
echo "=== 安装完成 ==="
echo "激活: bash tools/release/activate_release.sh $RELEASE_DIR"
echo "启动: sudo systemctl start hangzhouwan.target"
