#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
N=$HOME/.local/var/pmbootstrap/chroot_native
X="gcc4-armv7-alpine-linux-musleabihf-"
pmbootstrap -y chroot -- sh -c "cd /leo-ics && make -j\$(nproc) ARCH=arm CROSS_COMPILE=$X zImage 2>&1 | grep -E 'error:|Error [0-9]|\*\*\*|Kernel:' | head -25"
echo "=== результат ==="
sudo ls -l "$N/leo-ics/arch/arm/boot/zImage" 2>/dev/null || echo "zImage не собрался"
