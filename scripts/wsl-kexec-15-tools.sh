#!/bin/bash
# Собирает утилиту kexec статически под ARM.
# Статика обязательна: в Android ICS нет ни musl, ни свежей glibc.
set -u
export PATH="$HOME/.local/bin:$PATH"
N=$HOME/.local/var/pmbootstrap/chroot_native
X=gcc4-armv7-alpine-linux-musleabihf

pmbootstrap -y chroot -- sh -c "
cd /kexec-tools &&
CC=$X-gcc \
LD=$X-ld \
AR=$X-ar \
./configure --host=arm-linux --target=arm-linux \
  --without-zlib --without-lzma --without-xen \
  LDFLAGS=-static 2>&1 | tail -8
" 2>&1 | tail -12

echo "=== сборка ==="
pmbootstrap -y chroot -- sh -c "cd /kexec-tools && make -j\$(nproc) 2>&1 | grep -E 'error|Error|warning: .*static' | head -15"
echo "=== результат ==="
sudo ls -l "$N/kexec-tools/build/sbin/kexec" 2>/dev/null || sudo find "$N/kexec-tools" -name kexec -type f 2>/dev/null | head -3
