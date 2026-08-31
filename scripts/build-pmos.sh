#!/bin/sh
# Сборка postmarketOS для HTC HD2 (htc-leo). Запускать на Linux-машине.
#
#   ./build-pmos.sh              — init + сборка ядра
#   ./build-pmos.sh install /dev/sdX — записать rootfs на SD-карту
#
# htc-leo архивирован: в списке устройств pmbootstrap init его нет, вводите вручную
#   vendor:   htc
#   codename: leo
# UI: 3D-ускорения нет, берите console / xfce4 / mate. Phosh и Plasma Mobile не поедут.
set -eu

command -v pmbootstrap >/dev/null || {
	echo "pmbootstrap не найден: pipx install pmbootstrap (или пакет дистрибутива)" >&2
	exit 1
}

case "${1:-build}" in
build)
	pmbootstrap init
	# ядро 3.0.4 собирается принудительно gcc4, это долго
	pmbootstrap build --arch armv7 linux-htc-leo
	pmbootstrap build --arch armv7 device-htc-leo
	echo "Готово. Дальше: $0 install /dev/sdX"
	;;
install)
	dev="${2:?укажите блочное устройство SD-карты, например /dev/sdb}"
	[ -b "$dev" ] || { echo "$dev — не блочное устройство" >&2; exit 1; }
	echo "ВНИМАНИЕ: $dev будет полностью перезаписан."
	lsblk -o NAME,SIZE,MODEL,MOUNTPOINT "$dev" || true
	printf 'Продолжить? [yes/NO] '; read -r a; [ "$a" = yes ] || exit 1
	pmbootstrap install --sdcard="$dev"
	echo "Разделы: ${dev}1 = boot (FAT32), ${dev}2 = root (ext4) -> в системе телефона /dev/mmcblk0p2"
	;;
*)
	echo "usage: $0 [build|install /dev/sdX]" >&2; exit 2 ;;
esac
