#!/bin/sh
# Возврат рабочего ядра (gpiofix) в \PMOS\ + журнал зависшей загрузки.
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
cd /c/Projects/HTC-HD2-T8585 || exit 1
i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		sleep 3
		echo "=== журнал зависшей загрузки ==="
		MSYS_NO_PATHCONV=1 "$ADB" shell "cat /proc/last_kmsg" > logs/last_kmsg-rpckick.txt 2>&1
		tr -d '\r' < logs/last_kmsg-rpckick.txt | tail -12
		echo "=== откат ядра ==="
		MSYS_NO_PATHCONV=1 "$ADB" shell "
cd /sdcard/PMOS
cat zImage > zImage.rpcfix-hang
cat zImage.old2 > zImage
md5sum zImage zImage.old2
" 2>&1 | tr -d '\r'
		echo ОТКАЧЕНО
		exit 0
	fi
	sleep 5
done
echo "устройство не появилось"
