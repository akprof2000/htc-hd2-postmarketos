#!/bin/bash
set -u
N=$HOME/.local/var/pmbootstrap/chroot_native
C=/mnt/c/Projects/HTC-HD2-T8585
sudo cp "$N/leo-ics/arch/arm/boot/zImage" "$C/out/zImage-ics-kexec"
sudo chown $(id -u):$(id -g) "$C/out/zImage-ics-kexec"
ls -l "$C/out/zImage-ics-kexec"
