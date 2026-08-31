#!/bin/bash
# Ставит в native-chroot pmbootstrap старый компилятор gcc4, которым собрано
# ядро pmOS, и проверяет, что кросс-версия для armv7 на месте.
set -u
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap -y chroot -- apk add gcc4 gcc4-armv7 make bash bc perl xz 2>&1 | tail -5
echo "=== что появилось ==="
ls $HOME/.local/var/pmbootstrap/chroot_native/usr/bin/ | grep gcc4 | head -10
