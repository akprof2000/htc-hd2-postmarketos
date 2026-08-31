#!/bin/bash
# Добавляем безусловную печать разобранной карты памяти: надо понять, пуст ли
# список участков (тогда виновата разборка) или место действительно не ищется.
set -u
export PATH="$HOME/.local/bin:$PATH"
R=$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo
C=/mnt/c/Projects/HTC-HD2-T8585
F="$R/kexec-tools/kexec/arch/arm/kexec-arm.c"

sudo grep -q 'НАЙДЕНО УЧАСТКОВ' "$F" || sudo sed -i \
 's|\tdbgprint_mem_range("MEMORY RANGES", \*range, \*ranges);|\t{ int q; fprintf(stderr, "НАЙДЕНО УЧАСТКОВ: %d\n", *ranges); for (q = 0; q < *ranges; q++) fprintf(stderr, "  %d: %016llx-%016llx тип=%d\n", q, (unsigned long long)(*range)[q].start, (unsigned long long)(*range)[q].end, (*range)[q].type); }\n\tdbgprint_mem_range("MEMORY RANGES", *range, *ranges);|' "$F"
sudo grep -n 'НАЙДЕНО УЧАСТКОВ' "$F" | head -2

pmbootstrap -y chroot -r -- sh -c "cd /kexec-tools && make 2>&1 | tail -2"
K=$(sudo find "$R/kexec-tools" -name kexec -type f 2>/dev/null | head -1)
sudo cp "$K" "$C/out/kexec-arm-debug"
sudo chown $(id -u):$(id -g) "$C/out/kexec-arm-debug"
ls -l "$C/out/kexec-arm-debug"
