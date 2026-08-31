#!/bin/sh
# Переразметка SD-карты HTC HD2 под дуалбут WinMo/ICS + postmarketOS.
# Запускать в WSL после `wsl --mount \.\PHYSICALDRIVE<N> --bare` (PowerShell от админа).
#
#   ./prepare-sdcard.sh /dev/sdX [htc-leo-root.img] [htc-leo-boot.img]
#
# Раскладка:
#   p1  FAT32  8 ГБ    — видит Windows Mobile: \ICS\, \PMOS\, пользовательские файлы
#   p2  ext2   512 МБ  — метка pmOS_boot, сюда заливается htc-leo-boot.img
#   p3  ext4   остаток — метка pmOS_root, сюда заливается образ
#
# Про p2: htc-leo-boot.img, который выдаёт `pmbootstrap install --split`, - это
# ext2-образ на 512 МБ (внутри него лежат vmlinuz, initramfs и собранный Android
# boot image). Поэтому p2 сделан ровно 512 МБ и образ заливается dd.
# Для загрузки через HaRET этот раздел не обязателен - ядро и initramfs берутся с
# FAT32 напрямую, - но с ним штатно отработают будущие обновления ядра через apk.
#
# ВНИМАНИЕ: карта стирается целиком. Бэкап должен быть сделан ЗАРАНЕЕ
# (backup/sdcard + пройденный scripts/verify-backup.ps1).
set -eu

dev="${1:?укажите устройство карты, например /dev/sdd}"
root_img="${2:-}"
boot_img="${3:-}"

[ -b "$dev" ] || { echo "$dev — не блочное устройство" >&2; exit 1; }
case "$dev" in *[0-9]) echo "укажите диск (/dev/sdd), а не раздел" >&2; exit 1;; esac
for f in "$root_img" "$boot_img"; do
	[ -z "$f" ] || [ -f "$f" ] || { echo "нет файла $f" >&2; exit 1; }
done

echo "Цель: $dev"
lsblk -o NAME,SIZE,MODEL,LABEL,MOUNTPOINT "$dev"
cat <<MSG

Карта будет стёрта полностью, включая \ICS\ и все файлы.
Убедитесь, что backup/sdcard/ICS на месте и verify-backup.ps1 прошёл.
MSG
printf 'Продолжить? [yes/NO] '; read -r a; [ "$a" = yes ] || exit 1

umount "$dev"?* 2>/dev/null || true

parted -s "$dev" mklabel msdos
parted -s "$dev" mkpart primary fat32 1MiB 8GiB
parted -s "$dev" mkpart primary ext2  8GiB 8704MiB
parted -s "$dev" mkpart primary ext4  8704MiB 100%
parted -s "$dev" set 1 boot on
partprobe "$dev"; sleep 2

p() { case "$dev" in *mmcblk*|*nvme*|*loop*) echo "${dev}p$1";; *) echo "${dev}$1";; esac; }

mkfs.vfat -F 32 -n LEO "$(p 1)"
if [ -n "$boot_img" ]; then
	echo "Заливаю $boot_img -> $(p 2)"
	dd if="$boot_img" of="$(p 2)" bs=4M status=progress conv=fsync
	e2fsck -fy "$(p 2)" || true
	e2label "$(p 2)" pmOS_boot
else
	mkfs.ext2 -q -L pmOS_boot "$(p 2)"
fi

if [ -n "$root_img" ]; then
	echo "Заливаю $root_img -> $(p 3)"
	dd if="$root_img" of="$(p 3)" bs=4M status=progress conv=fsync
	e2fsck -fy "$(p 3)" || true
	resize2fs "$(p 3)"                 # растянуть ФС на весь раздел
	e2label "$(p 3)" pmOS_root         # метка обязательна: по ней initramfs ищет корень
else
	mkfs.ext4 -q -L pmOS_root "$(p 3)"
fi

sync
echo
lsblk -o NAME,SIZE,FSTYPE,LABEL "$dev"
cat <<MSG

Готово. Дальше:
  1. смонтировать $(p 1), вернуть туда backup/sdcard (включая \ICS\)
  2. создать на нём папку PMOS и положить payload из ./make-haret-payload.sh
  3. в Windows: wsl --unmount \\.\PHYSICALDRIVE<N>
MSG
