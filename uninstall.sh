#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -e

if [ "$EUID" -ne 0 ]; then
  echo "Error: Please run with sudo (e.g., sudo ./uninstall.sh)"
  exit 1
fi

TARGET_USER="${SUDO_USER:-$USER}"
TARGET_UID=$(id -u "$TARGET_USER")
KVER=$(uname -r)

manage_wireplumber() {
  local action=$1
  if [ -n "$TARGET_UID" ] && [ "$TARGET_UID" -ge 1000 ]; then
    sudo -u "$TARGET_USER" XDG_RUNTIME_DIR="/run/user/${TARGET_UID}" \
      systemctl --user "$action" wireplumber >/dev/null 2>&1 || true
  fi
}

echo "===> [1/4] Stopping WirePlumber..."
manage_wireplumber stop

echo "===> [2/4] Unloading module..."
if lsmod | grep -q "^snd_uac8"; then
  rmmod snd-uac8 || {
    echo "Warning: Module is currently in use. Please close JACK or your DAW and try again."
    manage_wireplumber start
    exit 1
  }
fi

echo "===> [3/4] Removing installed files and autoload configuration..."
rm -f "/lib/modules/${KVER}/extra/snd-uac8.ko"
rm -f "/lib/modules/${KVER}/kernel/drivers/sound/snd-uac8.ko"
rm -f /etc/modules-load.d/snd-uac8.conf

echo "===> [4/4] Updating module dependencies (depmod)..."
depmod -a

echo "===> Cleaning build artifacts..."
make clean >/dev/null 2>&1 || true

echo "===> Restarting WirePlumber..."
manage_wireplumber start

echo "----------------------------------------"
echo "Uninstallation complete!"
echo "----------------------------------------"
