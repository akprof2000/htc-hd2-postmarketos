#!/bin/sh
# Ждёт Android на кабеле и кладёт на карту загрузочную папку ICSK с ядром,
# поддерживающим kexec, плюс саму утилиту. Папка \ICS\ не трогается.
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
cd /c/Projects/HTC-HD2-T8585 || exit 1

i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		echo "$(date '+%H:%M:%S') устройство найдено"
		MSYS_NO_PATHCONV=1 "$ADB" shell "mkdir -p /sdcard/ICSK" >/dev/null 2>&1
		for f in haret.exe clrcad.exe startup.txt initrd.gz zImage; do
			MSYS_NO_PATHCONV=1 "$ADB" push "out/ICSK/$f" "/sdcard/ICSK/$f" 2>&1 | tail -1
		done
		if [ -f out/kexec-arm-static ]; then
			MSYS_NO_PATHCONV=1 "$ADB" push out/kexec-arm-static /sdcard/kexec 2>&1 | tail -1
		fi
		echo "--- что легло ---"
		MSYS_NO_PATHCONV=1 "$ADB" shell "ls -l /sdcard/ICSK/; ls -l /sdcard/kexec" 2>&1 | tr -d '\r'
		echo "--- сверка контрольных сумм ядра ---"
		MSYS_NO_PATHCONV=1 "$ADB" shell "md5sum /sdcard/ICSK/zImage" 2>&1 | tr -d '\r'
		md5sum out/ICSK/zImage
		echo "--- \ICS\ не тронут? ---"
		MSYS_NO_PATHCONV=1 "$ADB" shell "md5sum /sdcard/ICS/zImage" 2>&1 | tr -d '\r'
		md5sum backup/sdcard/ICS/zImage
		echo УСТАНОВЛЕНО
		exit 0
	fi
	sleep 5
done
echo "устройство не появилось за час"
