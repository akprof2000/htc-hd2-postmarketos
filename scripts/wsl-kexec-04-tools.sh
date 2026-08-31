#!/bin/bash
# Собирает kexec-tools статически под ARM тем же старым компилятором.
# Статика обязательна: в Android ICS нет нужных библиотек glibc/musl.
set -u
export PATH="$HOME/.local/bin:$PATH"
N=$HOME/.local/var/pmbootstrap/chroot_native
V=2.0.20   # последняя версия, ещё дружащая со старыми ядрами ARM

if [ ! -d "$HOME/kexec-tools-$V" ]; then
	cd "$HOME" || exit 1
	echo "качаю kexec-tools $V ..."
	timeout 600 curl -sSL -o kt.tar.xz \
		"https://mirrors.edge.kernel.org/pub/linux/utils/kernel/kexec/kexec-tools-$V.tar.xz" || exit 1
	ls -lh kt.tar.xz
	tar xf kt.tar.xz && rm -f kt.tar.xz
fi
echo "исходники: $(ls -d $HOME/kexec-tools-$V)"

sudo rm -rf "$N/kexec-tools"
sudo cp -a "$HOME/kexec-tools-$V" "$N/kexec-tools"
echo "скопировано в chroot"
