#!/bin/bash
# Приводим строку версии к оригинальной 2.6.32.15_tytung_ics_r1, чтобы модули
# Android (Wi-Fi, Bluetooth) грузились в новое ядро без пересборки.
set -u
export PATH="$HOME/.local/bin:$PATH"
N=$HOME/.local/var/pmbootstrap/chroot_native
X="gcc4-armv7-alpine-linux-musleabihf-"

echo "=== было в Makefile ==="
sudo grep -E '^(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION) ' "$N/leo-ics/Makefile"
sudo sed -i 's/^EXTRAVERSION = .*/EXTRAVERSION = .15/' "$N/leo-ics/Makefile"
echo "=== стало ==="
sudo grep -E '^(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION) ' "$N/leo-ics/Makefile"

echo "=== пересборка ==="
pmbootstrap -y chroot -- sh -c "cd /leo-ics && make -j\$(nproc) ARCH=arm CROSS_COMPILE=$X zImage 2>&1 | grep -E 'error:|Error [0-9]|Kernel:' | head -10"
sudo ls -l "$N/leo-ics/arch/arm/boot/zImage"
sudo cp "$N/leo-ics/arch/arm/boot/zImage" /mnt/c/Projects/HTC-HD2-T8585/out/zImage-ics-kexec
sudo chown $(id -u):$(id -g) /mnt/c/Projects/HTC-HD2-T8585/out/zImage-ics-kexec
