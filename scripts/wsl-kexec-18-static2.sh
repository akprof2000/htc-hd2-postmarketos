#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
R=$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo
C=/mnt/c/Projects/HTC-HD2-T8585

pmbootstrap -y chroot -r -- apk add linux-headers 2>&1 | tail -2
echo "=== сборка ==="
pmbootstrap -y chroot -r -- sh -c "cd /kexec-tools && make 2>&1 | grep -iE 'error|fatal' | head -10"
echo "=== результат ==="
sudo find "$R/kexec-tools" -name kexec -type f -exec ls -l {} \; 2>/dev/null | head -3
K=$(sudo find "$R/kexec-tools" -name kexec -type f 2>/dev/null | head -1)
if [ -n "$K" ]; then
	sudo cp "$K" "$C/out/kexec-arm-static"
	sudo chown $(id -u):$(id -g) "$C/out/kexec-arm-static"
	file "$C/out/kexec-arm-static"
fi
