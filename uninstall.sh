#!/bin/bash
set -e

# ルート権限チェック
if [ "$EUID" -ne 0 ]; then
  echo "エラー: sudo を付けて実行してください (例: sudo ./uninstall.sh)"
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

echo "===> [1/4] WirePlumber を一時停止中..."
manage_wireplumber stop

echo "===> [2/4] モジュールをアンロード中..."
if lsmod | grep -q "^snd_uac8"; then
  rmmod snd-uac8 || {
    echo "警告: モジュールが使用中です。JACK や DAW を終了してから再実行してください。"
    manage_wireplumber start
    exit 1
  }
fi

echo "===> [3/4] インストール済みファイルと自動設定を削除中..."
rm -f "/lib/modules/${KVER}/extra/snd-uac8.ko"
rm -f "/lib/modules/${KVER}/kernel/drivers/sound/snd-uac8.ko"
rm -f /etc/modules-load.d/snd-uac8.conf

echo "===> [4/4] モジュールの依存関係を更新中 (depmod)..."
depmod -a

echo "===> ビルド生成物をクリーンアップ中..."
make clean >/dev/null 2>&1 || true

echo "===> WirePlumber を再開中..."
manage_wireplumber start

echo "----------------------------------------"
echo "アンインストール完了！"
echo "----------------------------------------"
