#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
for try in 1 2 3; do
	echo "=== попытка $try ==="
	if pmbootstrap -y build --arch armv7 --force linux-htc-leo > /tmp/build.log 2>&1; then
		echo сборка_ок
		break
	fi
	tail -3 /tmp/build.log
	sleep 20
done
grep -E "03-gpio" /tmp/build.log | head -2
tail -4 /tmp/build.log
pmbootstrap -y install --split --password <TEMP_PASSWORD> >/dev/null 2>&1
sudo cp "$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo/boot/vmlinuz" /mnt/c/Projects/HTC-HD2-T8585/out/zImage-gpiofix
sudo chown "$(id -u):$(id -g)" /mnt/c/Projects/HTC-HD2-T8585/out/zImage-gpiofix
ls -l /mnt/c/Projects/HTC-HD2-T8585/out/zImage-gpiofix
