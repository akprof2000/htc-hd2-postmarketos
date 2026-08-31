#!/bin/bash
# gcc 4.7+ для ARMv6+ по умолчанию генерирует невыровненные обращения к памяти
# (-munaligned-access). Ядро 2.6.32 к этому не готово и падает с alignment
# exception в первые доли секунды. В ядрах 3.x флаг -mno-unaligned-access
# добавлен в arch/arm/Makefile явно; здесь его нет - дописываем.
set -u
export PATH="$HOME/.local/bin:$PATH"
N=$HOME/.local/var/pmbootstrap/chroot_native
C=/mnt/c/Projects/HTC-HD2-T8585
X="gcc4-armv7-alpine-linux-musleabihf-"

for T in leo-ics leo-ctl; do
	if ! sudo grep -q 'mno-unaligned-access' "$N/$T/arch/arm/Makefile"; then
		sudo sh -c "printf '\n# gcc 4.7+ иначе генерирует невыровненные обращения, которых ядро 2.6.32 не ждёт\nKBUILD_CFLAGS += -mno-unaligned-access\n' >> $N/$T/arch/arm/Makefile"
	fi
	echo "$T: $(sudo grep -c 'mno-unaligned-access' "$N/$T/arch/arm/Makefile") вхождений"
done

echo "=== пересборка ядра с kexec ==="
pmbootstrap -y chroot -- sh -c "cd /leo-ics && make -j\$(nproc) ARCH=arm CROSS_COMPILE=$X zImage 2>&1 | grep -E 'error:|Kernel:' | head -5"
sudo cp "$N/leo-ics/arch/arm/boot/zImage" "$C/out/zImage-ics-kexec"
sudo chown $(id -u):$(id -g) "$C/out/zImage-ics-kexec"
ls -l "$C/out/zImage-ics-kexec"
