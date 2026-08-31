#!/bin/sh
set -eu
export PATH="$HOME/.local/bin:$PATH"
src="$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo/boot"
out="/mnt/c/Projects/HTC-HD2-T8585/out/PMOS"
mkdir -p "$out"
sudo cp "$src/vmlinuz"   "$out/zImage"
sudo cp "$src/initramfs" "$out/initrd.gz"
sudo chown "$(id -u):$(id -g)" "$out/zImage" "$out/initrd.gz"
cp /mnt/c/Projects/HTC-HD2-T8585/out/pmos-startup.txt "$out/startup.txt"
# haret.exe и clrcad.exe - из проверенного бэкапа \ICS\
cp /mnt/c/Projects/HTC-HD2-T8585/backup/sdcard/ICS/haret.exe  "$out/"
cp /mnt/c/Projects/HTC-HD2-T8585/backup/sdcard/ICS/clrcad.exe "$out/"
echo "--- $out ---"
ls -lh "$out"
echo "--- тип файлов ---"
file "$out/zImage" "$out/initrd.gz" 2>/dev/null || true
