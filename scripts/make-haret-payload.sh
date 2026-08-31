#!/bin/sh
# Готовит payload для загрузки postmarketOS через HaRET на HTC HD2 (htc-leo).
#   ./make-haret-payload.sh [выходной_каталог]
# Значения mtype/ramaddr/ramsize взяты из рабочего \ICS\STARTUP.TXT этого аппарата.
set -eu

out="${1:-out/PMOS}"
RAMSIZE="${RAMSIZE:-0x1e400000}"        # из ICS/STARTUP.TXT, проверено на аппарате
RAMADDR="0x11800000"                    # = deviceinfo_flash_offset_base
INITRD_OFFSET="${INITRD_OFFSET:-0x01000000}"  # = deviceinfo_flash_offset_ramdisk
MTYPE="2524"                            # = MACH_HTCLEO
CMDLINE="no_console_suspend=1 wire.search_count=5 console=tty0 PMOS_NO_OUTPUT_REDIRECT"

mkdir -p "$out"
pmbootstrap export "$out/export"

k=$(find "$out/export" -maxdepth 1 -name 'vmlinuz*' | head -n1)
i=$(find "$out/export" -maxdepth 1 -name 'initramfs*' ! -name '*-extra*' | head -n1)
[ -n "$k" ] && [ -n "$i" ] || { echo "не нашёл vmlinuz/initramfs в $out/export" >&2; exit 1; }

cp -L "$k" "$out/zImage"
cp -L "$i" "$out/initrd.gz"

# HaRET читает startup.txt с CRLF - пишем как в оригинале
{
	printf 'set mtype %s\r\n' "$MTYPE"
	printf 'set ramaddr %s\r\n' "$RAMADDR"
	printf 'set ramsize %s\r\n' "$RAMSIZE"
	printf 'set initrd_offset %s\r\n' "$INITRD_OFFSET"
	printf 'set KERNEL zImage\r\n'
	printf 'set initrd initrd.gz\r\n'
	printf 'set cmdline "%s"\r\n' "$CMDLINE"
	printf 'boot\r\n'
} > "$out/startup.txt"

echo "Готово: $out"
ls -la "$out"
cat <<'MSG'

Дальше: скопировать zImage, initrd.gz, startup.txt в папку \PMOS\ на FAT32-разделе SD
и положить туда же haret.exe + clrcad.exe (копии из \ICS\). Папку \ICS\ не трогать.
MSG
