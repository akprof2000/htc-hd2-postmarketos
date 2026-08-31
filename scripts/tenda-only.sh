#!/bin/sh
# Оставляет в конфиге только Tenda: на Xiaomi у bcm4329 проходят рукопожатия,
# но не данные, и supplicant тратил все попытки там. Заодно чистит /data/local/tmp.
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
cd /c/Projects/HTC-HD2-T8585 || exit 1
i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		sleep 3
		MSYS_NO_PATHCONV=1 "$ADB" push out/wpa_supplicant.conf /sdcard/w.tmp >/dev/null 2>&1
		MSYS_NO_PATHCONV=1 "$ADB" shell "
mount -t ext4 -o rw /dev/block/mmcblk0p3 /data/local/pm
P=/data/local/pm
cat /sdcard/w.tmp > \$P/etc/wpa_supplicant/wpa_supplicant.conf
chmod 600 \$P/etc/wpa_supplicant/wpa_supplicant.conf
echo '--- сети в конфиге ---'
grep ssid \$P/etc/wpa_supplicant/wpa_supplicant.conf
echo '--- журнал прошлого сеанса ---'
tail -12 \$P/var/log/netdump.txt 2>/dev/null
/system/xbin/busybox rm -f /sdcard/w.tmp \$P/var/log/netdump.txt
/system/xbin/busybox rm -rf /data/local/tmp/*
echo '--- tmp очищен ---'
ls /data/local/tmp/ 2>/dev/null | wc -l
cd /; sync; umount \$P && echo размонтировано
" 2>&1 | tr -d '\r'
		echo ГОТОВО
		exit 0
	fi
	sleep 5
done
echo "устройство не появилось"
