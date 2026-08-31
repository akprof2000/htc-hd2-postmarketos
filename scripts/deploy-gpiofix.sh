#!/bin/sh
# Кладёт в \PMOS\ ядро с патчем 03 (сброс GPIO-прерываний до установки
# обработчика - лечение плавающего зависания на старте). Старое ядро
# сохраняется как zImage.old.
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
cd /c/Projects/HTC-HD2-T8585 || exit 1
i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		sleep 3
		MSYS_NO_PATHCONV=1 "$ADB" shell "cat /sdcard/PMOS/zImage > /sdcard/PMOS/zImage.old" 2>/dev/null
		MSYS_NO_PATHCONV=1 "$ADB" push out/zImage-gpiofix /sdcard/PMOS/zImage 2>&1 | tail -1
		echo "--- сверка ---"
		MSYS_NO_PATHCONV=1 "$ADB" shell "md5sum /sdcard/PMOS/zImage" 2>&1 | tr -d '\r'
		md5sum out/zImage-gpiofix
		echo УСТАНОВЛЕНО
		exit 0
	fi
	sleep 5
done
echo "устройство не появилось"
