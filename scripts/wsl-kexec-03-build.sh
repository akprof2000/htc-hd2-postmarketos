#!/bin/bash
# Собирает ядро ICS с kexec.
# Целевой код - gcc 4.9.4 (как ядро pmOS), хостовые утилиты - обычный gcc:
# у gcc4-gcc нет системных заголовков, и fixdep им не собирается.
set -u
export PATH="$HOME/.local/bin:$PATH"
X="gcc4-armv7-alpine-linux-musleabihf-"

pmbootstrap -y chroot -- apk add build-base linux-headers 2>&1 | tail -2

echo "=== oldconfig ==="
pmbootstrap -y chroot -- sh -c "cd /leo-ics && yes '' | make ARCH=arm CROSS_COMPILE=$X oldconfig" 2>&1 | tail -5

echo "=== KEXEC в итоговом .config ==="
pmbootstrap -y chroot -- sh -c "grep -E '^CONFIG_(KEXEC|ATAGS_PROC)' /leo-ics/.config"

echo "=== сборка zImage ==="
pmbootstrap -y chroot -- sh -c "cd /leo-ics && make -j\$(nproc) ARCH=arm CROSS_COMPILE=$X zImage" 2>&1 | tail -20
