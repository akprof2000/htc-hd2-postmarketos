#!/bin/sh
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
cd /c/Projects/HTC-HD2-T8585 || exit 1
i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		sleep 3
		MSYS_NO_PATHCONV=1 "$ADB" shell "
mount -t ext4 -o ro /dev/block/mmcblk0p3 /data/local/pm 2>/dev/null
cat /data/local/pm/var/log/netdump.txt 2>/dev/null
cd /; umount /data/local/pm
" 2>&1 | tr -d '\r'
		echo КОНЕЦ_ЛОГА
		exit 0
	fi
	sleep 5
done
echo "устройство не появилось"
