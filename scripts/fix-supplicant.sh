#!/bin/sh
# Убирает штатную службу wpa_supplicant из автозапуска pmOS: она поднимает
# второй процесс, который дерётся с нашим за интерфейс. В журнале точки доступа
# это выглядит как associated -> handshake completed -> disconnected в одну и ту
# же секунду, по нескольку раз подряд.
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
cd /c/Projects/HTC-HD2-T8585 || exit 1
i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		echo "$(date '+%H:%M:%S') устройство найдено"
		MSYS_NO_PATHCONV=1 "$ADB" shell "
mount -t ext4 -o rw /dev/block/mmcblk0p3 /data/local/pm
P=/data/local/pm
echo '--- журнал прошлого сеанса pmOS ---'
tail -25 \$P/var/log/netdump.txt 2>/dev/null || echo 'журнала нет'
echo '--- автозапуск ДО ---'
ls \$P/etc/runlevels/default/ | tr '\n' ' '
/system/xbin/busybox rm -f \$P/etc/runlevels/default/wpa_supplicant
echo ''
echo '--- автозапуск ПОСЛЕ ---'
ls \$P/etc/runlevels/default/ | tr '\n' ' '
echo ''
/system/xbin/busybox rm -f \$P/var/log/netdump.txt
cd /; sync; umount \$P && echo размонтировано
" 2>&1 | tr -d '\r'
		echo ГОТОВО
		exit 0
	fi
	sleep 5
done
echo "устройство не появилось"
