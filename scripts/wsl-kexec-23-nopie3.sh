#!/bin/bash
# Флаги линковки задаём ТОЛЬКО в configure: purgatory собирается особым образом
# (-r, перемещаемый объект), и статическая линковка его ломает.
set -u
export PATH="$HOME/.local/bin:$PATH"
R=$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo
C=/mnt/c/Projects/HTC-HD2-T8585

pmbootstrap -y chroot -r -- sh -c "cd /kexec-tools && make clean >/dev/null 2>&1; ./configure --without-zlib --without-lzma --without-xen CFLAGS='-O2 -fcommon -fno-pie' LDFLAGS='-static -no-pie' 2>&1 | tail -2"
pmbootstrap -y chroot -r -- sh -c "cd /kexec-tools && make 2>&1 | tail -3"
pmbootstrap -y chroot -r -- sh -c "strip /kexec-tools/build/sbin/kexec 2>/dev/null; true"
K=$(sudo find "$R/kexec-tools" -name kexec -type f 2>/dev/null | head -1)
if [ -n "$K" ]; then
	sudo cp "$K" "$C/out/kexec-arm-static"
	sudo chown $(id -u):$(id -g) "$C/out/kexec-arm-static"
	ls -l "$C/out/kexec-arm-static"; file "$C/out/kexec-arm-static"
else
	echo "не собрался - остаётся вариант static-pie"
fi
