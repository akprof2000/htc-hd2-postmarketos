#!/bin/sh
set -eu
export PATH="$HOME/.local/bin:$PATH"
cfg="$HOME/pmaports/device/archived/linux-htc-leo/config-htc-leo.armv7"
sed -i 's|^# CONFIG_DEVTMPFS_MOUNT is not set$|CONFIG_DEVTMPFS_MOUNT=y|' "$cfg"
grep -n "DEVTMPFS" "$cfg"
pmbootstrap checksum linux-htc-leo >/dev/null 2>&1
pmbootstrap -y build --arch armv7 --force linux-htc-leo 2>&1 | tail -2
pmbootstrap -y install --split --password <TEMP_PASSWORD> >/dev/null 2>&1
sudo cp "$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo/boot/vmlinuz" /mnt/c/Projects/HTC-HD2-T8585/out/zImage-noinitrd
sudo chown "$(id -u):$(id -g)" /mnt/c/Projects/HTC-HD2-T8585/out/zImage-noinitrd
ls -l /mnt/c/Projects/HTC-HD2-T8585/out/zImage-noinitrd
