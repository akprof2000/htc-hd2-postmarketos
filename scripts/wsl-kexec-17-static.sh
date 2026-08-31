#!/bin/bash
# Собирает kexec статически внутри armv7-окружения Alpine.
# Статический бинарник с musl запускается в Android без каких-либо библиотек.
# Версия 2.0.20, а не свежая 2.0.32: старые ядра ARM ей ближе.
set -u
export PATH="$HOME/.local/bin:$PATH"
R=$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo

echo "=== ставлю инструменты сборки в armv7-окружение ==="
pmbootstrap -y chroot -r -- apk add build-base zlib-static zlib-dev 2>&1 | tail -3

echo "=== переношу исходники ==="
sudo rm -rf "$R/kexec-tools"
sudo cp -a "$HOME/kexec-tools-2.0.20" "$R/kexec-tools"

echo "=== configure ==="
pmbootstrap -y chroot -r -- sh -c "cd /kexec-tools && ./configure --without-lzma --without-xen LDFLAGS=-static 2>&1 | tail -5"

echo "=== сборка ==="
pmbootstrap -y chroot -r -- sh -c "cd /kexec-tools && make 2>&1 | grep -iE 'error|warning: Using' | head -10"

echo "=== результат ==="
sudo find "$R/kexec-tools" -name 'kexec' -type f -exec ls -l {} \; 2>/dev/null | head -3
