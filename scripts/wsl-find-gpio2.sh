#!/bin/bash
# Дерево pmOS-ядра (3.0.101) - ищем шире
find $HOME/.local/var/pmbootstrap/cache_git -maxdepth 2 -type d 2>/dev/null | head
find $HOME/.local/var/pmbootstrap -maxdepth 8 -name "gpio-msm-v1.c" 2>/dev/null | head -3
find $HOME/.local/var/pmbootstrap/chroot_native -maxdepth 3 -type d -name "*htcleo*" 2>/dev/null | head
ls $HOME/.local/var/pmbootstrap/cache_distfiles 2>/dev/null | head
