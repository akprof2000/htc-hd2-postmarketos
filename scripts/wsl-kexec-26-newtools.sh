#!/bin/bash
# kexec-tools 2.0.32: в 2.0.20 разбор /proc/iomem на ARM работает ненадёжно.
set -u
export PATH="$HOME/.local/bin:$PATH"
R=$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo
C=/mnt/c/Projects/HTC-HD2-T8585
V=2.0.32

if [ ! -d "$HOME/kexec-tools-$V" ]; then
	cd "$HOME" || exit 1
	timeout 600 curl -sSL -o kt2.tar.xz \
		"https://mirrors.edge.kernel.org/pub/linux/utils/kernel/kexec/kexec-tools-$V.tar.xz" || exit 1
	tar xf kt2.tar.xz && rm -f kt2.tar.xz
fi
sudo rm -rf "$R/kexec-new"
sudo cp -a "$HOME/kexec-tools-$V" "$R/kexec-new"

pmbootstrap -y chroot -r -- sh -c "cd /kexec-new && ./configure --without-zlib --without-lzma --without-xen CFLAGS='-O2 -fcommon -fno-pie' LDFLAGS='-static -no-pie' 2>&1 | tail -2"
pmbootstrap -y chroot -r -- sh -c "cd /kexec-new && make 2>&1 | tail -3"
pmbootstrap -y chroot -r -- sh -c "strip /kexec-new/build/sbin/kexec 2>/dev/null; true"
K=$(sudo find "$R/kexec-new" -name kexec -type f 2>/dev/null | head -1)
if [ -n "$K" ]; then
	sudo cp "$K" "$C/out/kexec-arm-232"
	sudo chown $(id -u):$(id -g) "$C/out/kexec-arm-232"
	ls -l "$C/out/kexec-arm-232"; file "$C/out/kexec-arm-232"
else
	echo "не собрался"
fi
