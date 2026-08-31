#!/bin/bash
for d in $HOME/.local/var/pmbootstrap/cache_git/* $HOME/.local/var/pmbootstrap/chroot_native/home/pmos/* ; do
  [ -e "$d/arch/arm/mach-msm" ] && echo "ДЕРЕВО: $d"
done
find $HOME/.local/var/pmbootstrap -maxdepth 6 -name "gpio.c" -path "*mach-msm*" 2>/dev/null | head -3
