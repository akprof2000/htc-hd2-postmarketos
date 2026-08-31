#!/bin/sh
# Применяет патч VIC (обработка 0xFFFF) к пакету ядра и пересобирает.
set -eu
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
P=02-vic-spurious-irq-0xffff.patch

cp "/mnt/c/Projects/HTC-HD2-T8585/out/$P" "$A/$P"

# добавить патч в source=, если его там ещё нет
if ! grep -q "$P" "$A/APKBUILD"; then
	awk -v p="$P" '
		/^\t01-fix-compiler-path\.patch$/ { print; print "\t" p; next }
		{ print }
	' "$A/APKBUILD" > "$A/APKBUILD.new" && mv "$A/APKBUILD.new" "$A/APKBUILD"
fi

echo "=== source= в APKBUILD ==="
sed -n '/^source=/,/^"/p' "$A/APKBUILD"

pmbootstrap checksum linux-htc-leo
echo "=== сборка ==="
pmbootstrap -y build --arch armv7 --force linux-htc-leo 2>&1 | tail -4
pmbootstrap -y install --split --password <TEMP_PASSWORD> >/dev/null 2>&1
sudo cp "$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo/boot/vmlinuz" \
	/mnt/c/Projects/HTC-HD2-T8585/out/zImage-irqfix
sudo chown "$(id -u):$(id -g)" /mnt/c/Projects/HTC-HD2-T8585/out/zImage-irqfix
ls -l /mnt/c/Projects/HTC-HD2-T8585/out/zImage-irqfix
