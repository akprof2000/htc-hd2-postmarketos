#!/bin/sh
# Собирает готовый образ SD-карты для HTC HD2 в ФАЙЛЕ (работа только с loop-устройством,
# физические диски не затрагиваются). Результат пишется рядом и потом записывается на
# карту обычным писателем образов (Rufus в режиме DD / balenaEtcher).
#
# Раскладка образа (12,5 ГиБ):
#   p1  FAT32  8 ГиБ    метка LEO       — то, что видит Windows Mobile: \ICS\, \PMOS\
#   p2  ext2   512 МиБ  метка pmOS_boot — htc-leo-boot.img
#   p3  ext4   ~4 ГиБ   метка pmOS_root — htc-leo-root.img, растянут на весь раздел
set -eu

IMGDIR="$HOME/.local/var/pmbootstrap/chroot_native/home/pmos/rootfs"
OUT="$HOME/htc-leo-sdcard.img"
SIZE_MIB=12800

sudo test -f "$IMGDIR/htc-leo-root.img" || { echo "нет htc-leo-root.img" >&2; exit 1; }
sudo test -f "$IMGDIR/htc-leo-boot.img" || { echo "нет htc-leo-boot.img" >&2; exit 1; }

rm -f "$OUT"
truncate -s "${SIZE_MIB}M" "$OUT"

lo=$(sudo losetup --show -f -P "$OUT")
echo "loop: $lo"
cleanup() { sudo losetup -d "$lo" 2>/dev/null || true; }
trap cleanup EXIT

sudo parted -s "$lo" mklabel msdos
sudo parted -s "$lo" mkpart primary fat32 1MiB 8193MiB
sudo parted -s "$lo" mkpart primary ext2  8193MiB 8705MiB
sudo parted -s "$lo" mkpart primary ext4  8705MiB 100%
sudo parted -s "$lo" set 1 boot on
sudo partprobe "$lo" 2>/dev/null || true
sleep 1

sudo mkfs.vfat -F 32 -n LEO "${lo}p1"

echo "=== boot -> ${lo}p2 ==="
sudo dd if="$IMGDIR/htc-leo-boot.img" of="${lo}p2" bs=4M conv=fsync status=none
sudo e2fsck -fy "${lo}p2" >/dev/null 2>&1 || true
sudo e2label "${lo}p2" pmOS_boot

echo "=== root -> ${lo}p3 ==="
sudo dd if="$IMGDIR/htc-leo-root.img" of="${lo}p3" bs=4M conv=fsync status=none
sudo e2fsck -fy "${lo}p3" >/dev/null 2>&1 || true
sudo resize2fs "${lo}p3"
sudo e2label "${lo}p3" pmOS_root

echo "=== кладу \PMOS\ на FAT32 ==="
mnt=$(mktemp -d)
sudo mount "${lo}p1" "$mnt"
sudo mkdir -p "$mnt/PMOS"
# cp -r, не -a: на FAT32 нет владельцев, -a падает на chown
sudo cp -r /mnt/c/Projects/HTC-HD2-T8585/out/PMOS/. "$mnt/PMOS/"
sudo sync
ls -lh "$mnt/PMOS"
sudo umount "$mnt"; rmdir "$mnt"

echo "=== итоговая разметка образа ==="
sudo parted -s "$lo" print
lsblk -o NAME,SIZE,FSTYPE,LABEL "$lo"
cleanup; trap - EXIT
ls -lh "$OUT"
echo "=== ОБРАЗ ГОТОВ: $OUT ==="
