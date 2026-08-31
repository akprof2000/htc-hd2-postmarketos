#!/bin/bash
# Ищет пригодный кросс-компилятор для ядра 2.6.32 (нужен старый gcc).
N=$HOME/.local/var/pmbootstrap/chroot_native
echo "=== native-chroot существует: $([ -d "$N" ] && echo да || echo нет) ==="
echo "--- gcc внутри chroot ---"
ls "$N/usr/bin" 2>/dev/null | grep -E '^(.*-)?gcc' | head -20
echo "--- пакеты apk с gcc ---"
grep -h '^P:' "$N/lib/apk/db/installed" 2>/dev/null | grep -i gcc | head
echo "--- другие chroot ---"
ls -d $HOME/.local/var/pmbootstrap/chroot_* 2>/dev/null
echo "=== pmbootstrap доступен? ==="
which pmbootstrap || ls $HOME/.local/bin/pmbootstrap 2>/dev/null
echo "=== пакет gcc4.9-armv7 в pmaports ==="
ls -d $HOME/pmaports/cross/gcc4.9-armv7 2>/dev/null && grep -E '^pkgver|^pkgrel' $HOME/pmaports/cross/gcc4.9-armv7/APKBUILD 2>/dev/null
