#!/bin/sh
# Кладёт контрольную папку ICSC (то же ядро, но без kexec и atags).
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
cd /c/Projects/HTC-HD2-T8585 || exit 1
i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		echo "$(date '+%H:%M:%S') устройство найдено"
		MSYS_NO_PATHCONV=1 "$ADB" shell "mkdir -p /sdcard/ICSC" >/dev/null 2>&1
		for f in haret.exe clrcad.exe startup.txt initrd.gz zImage; do
			MSYS_NO_PATHCONV=1 "$ADB" push "out/ICSC/$f" "/sdcard/ICSC/$f" >/dev/null 2>&1
		done
		echo "--- что легло ---"
		MSYS_NO_PATHCONV=1 "$ADB" shell "ls -l /sdcard/ICSC/" 2>&1 | tr -d '\r'
		MSYS_NO_PATHCONV=1 "$ADB" shell "md5sum /sdcard/ICSC/zImage" 2>&1 | tr -d '\r'
		md5sum out/ICSC/zImage
		echo "--- журнал прошлой загрузки ---"
		MSYS_NO_PATHCONV=1 "$ADB" shell "cat /proc/version; echo ---; cat /proc/last_kmsg 2>/dev/null | tail -15" 2>&1 | tr -d '\r'
		echo УСТАНОВЛЕНО
		exit 0
	fi
	sleep 5
done
echo "устройство не появилось за час"
