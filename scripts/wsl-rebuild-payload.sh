#!/bin/sh
set -eu
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap -y install --split --password <TEMP_PASSWORD>
src="$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo/boot"
out="/mnt/c/Projects/HTC-HD2-T8585/out/PMOS"
sudo cp "$src/vmlinuz"   "$out/zImage"
sudo cp "$src/initramfs" "$out/initrd.gz"
sudo chown "$(id -u):$(id -g)" "$out/zImage" "$out/initrd.gz"
echo "=== новый payload ==="
ls -l "$out"
file "$out/zImage"
