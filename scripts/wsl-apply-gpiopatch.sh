#!/bin/bash
# Добавляет патч 03 (сброс GPIO-прерываний до установки обработчика) и пересобирает.
set -eu
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
P=03-gpio-clear-before-handler.patch
cp "/mnt/c/Projects/HTC-HD2-T8585/out/$P" "$A/$P"
if ! grep -q "$P" "$A/APKBUILD"; then
	awk -v p="$P" '
		/^\t02-vic-spurious-irq-0xffff\.patch$/ { print; print "\t" p; next }
		{ print }
	' "$A/APKBUILD" > "$A/APKBUILD.new" && mv "$A/APKBUILD.new" "$A/APKBUILD"
fi
sed -n '/^source=/,/^"/p' "$A/APKBUILD"
pmbootstrap checksum linux-htc-leo >/dev/null 2>&1
echo "=== сборка ==="
pmbootstrap -y build --arch armv7 --force linux-htc-leo 2>&1 | tail -4
pmbootstrap -y install --split --password <TEMP_PASSWORD> >/dev/null 2>&1
sudo cp "$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo/boot/vmlinuz" /mnt/c/Projects/HTC-HD2-T8585/out/zImage-gpiofix
sudo chown "$(id -u):$(id -g)" /mnt/c/Projects/HTC-HD2-T8585/out/zImage-gpiofix
ls -l /mnt/c/Projects/HTC-HD2-T8585/out/zImage-gpiofix
