#!/bin/bash
# Готовит дерево ядра ICS внутри chroot и конфигурацию с включённым kexec.
set -u
N=$HOME/.local/var/pmbootstrap/chroot_native
SRC=$HOME/leo-ics
C=/mnt/c/Projects/HTC-HD2-T8585

echo "=== исходники ==="
cd "$SRC" || exit 1
git log --oneline -1
grep -E '^(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION)' Makefile | tr '\n' ' '; echo
echo "--- есть ли поддержка kexec в дереве ---"
ls arch/arm/kernel/machine_kexec.c arch/arm/kernel/relocate_kernel.S 2>/dev/null
grep -n 'config KEXEC' arch/arm/Kconfig | head -3

echo "=== копирую дерево в chroot ==="
sudo rm -rf "$N/leo-ics"
sudo cp -a "$SRC" "$N/leo-ics"

echo "=== кладу конфигурацию работающего ядра + KEXEC ==="
sudo cp "$C/out/config-ics-tytung" "$N/leo-ics/.config"
sudo sh -c 'sed -i "/CONFIG_KEXEC/d" '"$N"'/leo-ics/.config'
sudo sh -c 'printf "CONFIG_KEXEC=y\nCONFIG_ATAGS_PROC=y\n" >> '"$N"'/leo-ics/.config'
echo "строк в .config: $(sudo wc -l < "$N/leo-ics/.config")"
sudo grep -c . "$N/leo-ics/.config" >/dev/null && sudo grep 'KEXEC\|ATAGS' "$N/leo-ics/.config"
