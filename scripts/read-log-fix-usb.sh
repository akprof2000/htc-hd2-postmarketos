#!/bin/sh
# 1) Читает журнал прошлого сеанса pmOS.
# 2) Переключает USB Android в режим "только adb" (usb_function_switch=2),
#    чтобы Windows не видела карту и не предлагала форматировать ext4.
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
cd /c/Projects/HTC-HD2-T8585 || exit 1
i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		echo "$(date '+%H:%M:%S') устройство найдено"
		MSYS_NO_PATHCONV=1 "$ADB" shell "
echo '--- USB: было ---'
cat /sys/devices/platform/msm_hsusb/usb_function_switch 2>/dev/null
echo 2 > /sys/devices/platform/msm_hsusb/usb_function_switch 2>/dev/null
echo '--- USB: стало ---'
cat /sys/devices/platform/msm_hsusb/usb_function_switch 2>/dev/null
mount -t ext4 -o ro /dev/block/mmcblk0p3 /data/local/pm 2>/dev/null
echo '--- журнал pmOS ---'
cat /data/local/pm/var/log/netdump.txt 2>/dev/null | head -30
cd /; umount /data/local/pm
" 2>&1 | tr -d '\r'
		echo ГОТОВО
		exit 0
	fi
	sleep 5
done
echo "устройство не появилось"
