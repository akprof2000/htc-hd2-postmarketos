#!/bin/sh
# Ждёт появления Android на кабеле и разворачивает на карту pmOS всё нужное:
# самовосстанавливающийся скрипт сети и SSH-ключ для входа без пароля.
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
cd /c/Projects/HTC-HD2-T8585 || exit 1

i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		echo "$(date '+%H:%M:%S') устройство найдено, разворачиваю"
		MSYS_NO_PATHCONV=1 "$ADB" push out/netdump.start /sdcard/n.tmp >/dev/null 2>&1
		MSYS_NO_PATHCONV=1 "$ADB" push out/ssh/openwrt_key.pub /sdcard/k.tmp >/dev/null 2>&1
		MSYS_NO_PATHCONV=1 "$ADB" shell "
mount -t ext4 -o rw /dev/block/mmcblk0p3 /data/local/pm
P=/data/local/pm
cat /sdcard/n.tmp > \$P/etc/local.d/netdump.start
chmod 755 \$P/etc/local.d/netdump.start
mkdir -p \$P/root/.ssh \$P/home/user/.ssh
cat /sdcard/k.tmp > \$P/root/.ssh/authorized_keys
cat /sdcard/k.tmp > \$P/home/user/.ssh/authorized_keys
chmod 700 \$P/root/.ssh \$P/home/user/.ssh
chmod 600 \$P/root/.ssh/authorized_keys \$P/home/user/.ssh/authorized_keys
/system/xbin/busybox chown -R 0:0 \$P/root/.ssh
/system/xbin/busybox chown -R 10000:10000 \$P/home/user/.ssh
/system/xbin/busybox rm -f /sdcard/n.tmp /sdcard/k.tmp \$P/var/log/netdump.txt
echo '--- проверка ---'
sh -n \$P/etc/local.d/netdump.start && echo 'скрипт: синтаксис ок'
grep -c ssh-ed25519 \$P/root/.ssh/authorized_keys | sed 's/^/ключей у root: /'
ls \$P/etc/runlevels/default/ | tr '\n' ' '
cd /; sync; umount \$P && echo РАЗВЁРНУТО
" 2>&1 | tr -d '\r'
		exit 0
	fi
	sleep 5
done
echo "устройство так и не появилось за час"
