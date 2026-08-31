#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
R=$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo
sudo cp /mnt/c/Projects/HTC-HD2-T8585/out/beep.c "$R/tmp/beep.c"
pmbootstrap -y chroot -r -- sh -c "gcc -O2 -static -o /tmp/beep /tmp/beep.c -lm && strip /tmp/beep && ls -l /tmp/beep" 2>&1 | tail -2
sudo cp "$R/tmp/beep" /mnt/c/Projects/HTC-HD2-T8585/out/beep 2>/dev/null
sudo chown $(id -u):$(id -g) /mnt/c/Projects/HTC-HD2-T8585/out/beep
ls -l /mnt/c/Projects/HTC-HD2-T8585/out/beep
