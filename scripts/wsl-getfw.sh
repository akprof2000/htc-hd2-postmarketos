#!/bin/sh
set -eu
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap -y chroot -r -- apk add firmware-aosp-broadcom-wlan 2>&1 | tail -6
R="$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo"
echo "=== файлы прошивки ==="
sudo find "$R/lib/firmware" -iname "*4329*" 2>/dev/null
echo "=== ожидаемый ядром путь ==="
sudo ls -l "$R/lib/firmware/postmarketos/bcmdhd/bcm4329/" 2>&1
