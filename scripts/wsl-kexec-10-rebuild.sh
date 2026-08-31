#!/bin/bash
# Полный проход: точный тег ics_r1, правка флагов секций, конфиг с KEXEC, сборка.
set -u
export PATH="$HOME/.local/bin:$PATH"
N=$HOME/.local/var/pmbootstrap/chroot_native
C=/mnt/c/Projects/HTC-HD2-T8585
X="gcc4-armv7-alpine-linux-musleabihf-"

echo "=== клонирую тег ics_r1 ==="
rm -rf $HOME/leo-ics
cd $HOME || exit 1
timeout 900 git clone --depth 1 --branch ics_r1 \
	https://github.com/tytung/android_kernel_htcleo-2.6.32.git leo-ics 2>&1 | tail -2
cd $HOME/leo-ics || exit 1
git log --oneline -1

echo "=== правлю флаги секций под современный ассемблер ==="
find . \( -name '*.S' -o -name '*.h' \) -print0 | xargs -0 sed -i \
	-e 's/\(\.section[^,]*\), *#alloc, *#execinstr/\1, "ax"/g' \
	-e 's/\(\.section[^,]*\), *#alloc, *#write/\1, "aw"/g' \
	-e 's/\(\.section[^,]*\), *#alloc/\1, "a"/g'
echo "осталось старых записей: $(grep -r '\.section.*#alloc' --include=*.S . 2>/dev/null | wc -l)"

echo "=== переношу в chroot ==="
sudo rm -rf "$N/leo-ics"
sudo cp -a $HOME/leo-ics "$N/leo-ics"
sudo cp "$C/out/config-ics-tytung" "$N/leo-ics/.config"
sudo sh -c "sed -i '/CONFIG_KEXEC/d' $N/leo-ics/.config"
sudo sh -c "printf 'CONFIG_KEXEC=y\nCONFIG_ATAGS_PROC=y\n' >> $N/leo-ics/.config"

echo "=== oldconfig ==="
pmbootstrap -y chroot -- sh -c "cd /leo-ics && yes '' | make ARCH=arm CROSS_COMPILE=$X oldconfig" 2>&1 | tail -3
pmbootstrap -y chroot -- sh -c "grep -E '^CONFIG_(KEXEC|ATAGS_PROC)' /leo-ics/.config"

echo "=== сборка ==="
pmbootstrap -y chroot -- sh -c "cd /leo-ics && make -j\$(nproc) ARCH=arm CROSS_COMPILE=$X zImage 2>&1 | grep -E 'error:|Error [0-9]|\*\*\*|Kernel:' | head -25"
echo "=== результат ==="
sudo ls -l "$N/leo-ics/arch/arm/boot/zImage" 2>/dev/null || echo "zImage не собрался"
