#!/bin/sh
# 1) чинит защиту от двойного supplicant (pgrep -c нет в BusyBox)
# 2) запрещает supplicant переписывать наш конфиг (update_config=0)
# 3) отключает автозапуск wpa_supplicant через D-Bus
# 4) показывает, что такое routewrangler - подозреваемый в запуске второго процесса
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
cd /c/Projects/HTC-HD2-T8585 || exit 1
i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		sleep 3
		MSYS_NO_PATHCONV=1 "$ADB" push out/netdump.start /sdcard/n.tmp >/dev/null 2>&1
		MSYS_NO_PATHCONV=1 "$ADB" push out/wpa_supplicant.conf /sdcard/w.tmp >/dev/null 2>&1
		MSYS_NO_PATHCONV=1 "$ADB" shell "
mount -t ext4 -o rw /dev/block/mmcblk0p3 /data/local/pm
P=/data/local/pm
cat /sdcard/n.tmp > \$P/etc/local.d/netdump.start; chmod 755 \$P/etc/local.d/netdump.start
cat /sdcard/w.tmp > \$P/etc/wpa_supplicant/wpa_supplicant.conf; chmod 600 \$P/etc/wpa_supplicant/wpa_supplicant.conf
echo '--- конфиг на карте (был ли переписан прошлым supplicant?) - до замены прочитаем позже, сейчас содержимое: ---'
grep -E 'ssid|update_config' \$P/etc/wpa_supplicant/wpa_supplicant.conf
echo '--- D-Bus автозапуск supplicant ---'
ls \$P/usr/share/dbus-1/system-services/ 2>/dev/null
for f in \$P/usr/share/dbus-1/system-services/fi.w1.wpa_supplicant1.service; do
  [ -f \$f ] && /system/xbin/busybox mv \$f \$f.off && echo \"отключён: \$f\"
done
echo '--- routewrangler ---'
head -30 \$P/etc/init.d/routewrangler 2>/dev/null
/system/xbin/busybox rm -f /sdcard/n.tmp /sdcard/w.tmp \$P/var/log/netdump.txt
cd /; sync; umount \$P && echo размонтировано
" 2>&1 | tr -d '\r'
		echo ГОТОВО
		exit 0
	fi
	sleep 5
done
echo "устройство не появилось"
