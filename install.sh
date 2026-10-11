#!/bin/bash
set -e

# ルート権限チェック
if [ "$EUID" -ne 0 ]; then
  echo "エラー: sudo を付けて実行してください (例: sudo ./install.sh)"
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

echo "===> [1/5] WirePlumber を一時停止中..."
manage_wireplumber stop

echo "===> [2/5] snd-uac8 をビルド中..."
make clean
make

echo "===> [3/5] モジュールを ${INSTALL_DIR} に配置中..."
mkdir -p "${INSTALL_DIR}"
cp snd-uac8.ko "${INSTALL_DIR}/"

echo "===> [4/5] モジュールの依存関係を更新中 (depmod)..."
depmod -a

echo "===> [5/5] 自動ロード設定 (/etc/modules-load.d/snd-uac8.conf) を作成中..."
echo "snd-uac8" > /etc/modules-load.d/snd-uac8.conf

echo "===> ドライバを即時ロード中..."
if lsmod | grep -q "^snd_uac8"; then
  rmmod snd-uac8 || true
fi
modprobe snd-uac8

echo "===> WirePlumber を再開中..."
manage_wireplumber start

echo "----------------------------------------"
echo "インストール完了！"
echo "カードの認識状態:"
cat /proc/asound/cards | grep -i uac || true
echo "----------------------------------------"
