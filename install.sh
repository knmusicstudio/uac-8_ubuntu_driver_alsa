#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -e

if [ "$EUID" -ne 0 ]; then
  echo "Error: Please run with sudo (e.g., sudo ./install.sh)"
  exit 1
fi

TARGET_USER="${SUDO_USER:-$USER}"
TARGET_UID=$(id -u "$TARGET_USER")
KVER=$(uname -r)
INSTALL_DIR="/lib/modules/${KVER}/extra"

manage_wireplumber() {
  local action=$1
  if [ -n "$TARGET_UID" ] && [ "$TARGET_UID" -ge 1000 ]; then
    sudo -u "$TARGET_USER" XDG_RUNTIME_DIR="/run/user/${TARGET_UID}" \
      systemctl --user "$action" wireplumber >/dev/null 2>&1 || true
  fi
}

echo "===> [1/5] Stopping WirePlumber..."
manage_wireplumber stop

echo "===> [2/5] Building snd-uac8..."
make clean
make

echo "===> [3/5] Installing module to ${INSTALL_DIR}..."
mkdir -p "${INSTALL_DIR}"
cp snd-uac8.ko "${INSTALL_DIR}/"

echo "===> [4/5] Updating module dependencies (depmod)..."
depmod -a

echo "===> [5/5] Configuring module autoload (/etc/modules-load.d/snd-uac8.conf)..."
echo "snd-uac8" > /etc/modules-load.d/snd-uac8.conf

echo "===> Reloading driver..."
modprobe -r snd-uac8 2>/dev/null || true
modprobe snd-uac8

echo "===> Restarting WirePlumber..."
manage_wireplumber start

echo "----------------------------------------"
echo "Installation complete!"
echo "Detected sound cards:"
grep -i uac /proc/asound/cards || true
echo "----------------------------------------"
