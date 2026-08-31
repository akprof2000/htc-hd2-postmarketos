#!/bin/sh
# Загружает pmOS из работающего Android через kexec.
#
# --atags обязателен для этой платформы: ядро 2.6.32/3.0 на QSD8x50 получает
# параметры от загрузчика списком ATAG, а не деревом устройств. Утилита читает
# их из /proc/atags (для этого в ядро добавлен CONFIG_ATAGS_PROC) и передаёт
# следующему ядру, подменяя только строку команд.
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
CMDLINE="root=/dev/mmcblk0p3 rootwait rw lpj=499435 ignore_loglevel"

if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -eq 0 ]; then
	echo "Android не подключён"; exit 1
fi

echo "=== подготовка ==="
MSYS_NO_PATHCONV=1 "$ADB" shell "
cat /sdcard/kexec > /data/local/tmp/kexec 2>/dev/null
chmod 755 /data/local/tmp/kexec
cat /sdcard/PMOS/zImage > /data/local/tmp/zImage
ls -l /data/local/tmp/kexec /data/local/tmp/zImage
" 2>&1 | tr -d '\r'

echo "=== загружаю образ pmOS ==="
MSYS_NO_PATHCONV=1 "$ADB" shell "/data/local/tmp/kexec -l /data/local/tmp/zImage --command-line='$CMDLINE' --atags 2>&1; echo код=\$?" 2>&1 | tr -d '\r'

echo "=== состояние ==="
MSYS_NO_PATHCONV=1 "$ADB" shell "cat /sys/kernel/kexec_loaded 2>/dev/null | sed 's/^/kexec_loaded=/'" 2>&1 | tr -d '\r'
