#!/bin/sh
# Удаляет с карты контрольную папку ICSC: она больше не нужна, причина
# незагрузки найдена (невыровненный доступ), сравнивать не с чем.
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		echo "$(date '+%H:%M:%S') устройство найдено"
		MSYS_NO_PATHCONV=1 "$ADB" shell "
/system/xbin/busybox rm -rf /sdcard/ICSC
echo '--- что осталось на карте ---'
ls -d /sdcard/ICS /sdcard/ICSK /sdcard/ICSC /sdcard/PMOS 2>/dev/null
" 2>&1 | tr -d '\r'
		echo УДАЛЕНО
		exit 0
	fi
	sleep 5
done
echo "устройство не появилось"
