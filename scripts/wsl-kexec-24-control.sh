#!/bin/bash
# Контрольная сборка: то же дерево, та же конфигурация, БЕЗ kexec и atags.
# Если она тоже не грузится - виноват компилятор, а не наши опции.
set -u
export PATH="$HOME/.local/bin:$PATH"
N=$HOME/.local/var/pmbootstrap/chroot_native
C=/mnt/c/Projects/HTC-HD2-T8585
X="gcc4-armv7-alpine-linux-musleabihf-"

sudo rm -rf "$N/leo-ctl"
sudo cp -a "$N/leo-ics" "$N/leo-ctl"
sudo sh -c "cd $N/leo-ctl && make ARCH=arm mrproper >/dev/null 2>&1; true"
sudo cp "$C/out/config-ics-tytung" "$N/leo-ctl/.config"
sudo sed -i 's/^EXTRAVERSION = .*/EXTRAVERSION = .15/' "$N/leo-ctl/Makefile"

pmbootstrap -y chroot -- sh -c "cd /leo-ctl && yes '' | make ARCH=arm CROSS_COMPILE=$X oldconfig >/dev/null 2>&1; grep -c KEXEC .config"
pmbootstrap -y chroot -- sh -c "cd /leo-ctl && make -j\$(nproc) ARCH=arm CROSS_COMPILE=$X zImage 2>&1 | grep -E 'error:|Kernel:' | head -5"
sudo cp "$N/leo-ctl/arch/arm/boot/zImage" "$C/out/zImage-ics-control" 2>/dev/null
sudo chown $(id -u):$(id -g) "$C/out/zImage-ics-control" 2>/dev/null
ls -l "$C/out/zImage-ics-control" 2>/dev/null || echo "не собралось"
