#!/bin/sh
set -eu
export PATH="$HOME/.local/bin:$PATH"
cfg="$HOME/pmaports/device/archived/linux-htc-leo/config-htc-leo.armv7"
sed -i 's|^CONFIG_VT_CONSOLE=y$|# CONFIG_VT_CONSOLE is not set|' "$cfg"
grep -n "^CONFIG_VT=\|VT_CONSOLE" "$cfg"
pmbootstrap checksum linux-htc-leo >/dev/null 2>&1
pmbootstrap -y build --arch armv7 --force linux-htc-leo 2>&1 | tail -2
pmbootstrap -y install --split --password <TEMP_PASSWORD> >/dev/null 2>&1
sudo cp "$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo/boot/vmlinuz" /mnt/c/Projects/HTC-HD2-T8585/out/zImage-novtcon
sudo chown "$(id -u):$(id -g)" /mnt/c/Projects/HTC-HD2-T8585/out/zImage-novtcon
ls -l /mnt/c/Projects/HTC-HD2-T8585/out/zImage-novtcon
