#!/bin/sh
# Ждёт Android и проверяет готовность к kexec.
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
cd /c/Projects/HTC-HD2-T8585 || exit 1
i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		echo "=== какое ядро работает ==="
		MSYS_NO_PATHCONV=1 "$ADB" shell "cat /proc/version" 2>&1 | tr -d '\r'
		echo "=== /proc/atags ==="
		MSYS_NO_PATHCONV=1 "$ADB" shell "ls -l /proc/atags 2>/dev/null || echo НЕТ" 2>&1 | tr -d '\r'
		echo "=== признаки kexec в ядре ==="
		MSYS_NO_PATHCONV=1 "$ADB" shell "cat /sys/kernel/kexec_loaded 2>/dev/null | sed 's/^/kexec_loaded=/'; ls /sys/kernel/ 2>/dev/null | grep -i kexec" 2>&1 | tr -d '\r'
		echo "=== утилита ==="
		MSYS_NO_PATHCONV=1 "$ADB" shell "cat /sdcard/kexec > /data/local/tmp/kexec; chmod 755 /data/local/tmp/kexec; /data/local/tmp/kexec --version 2>&1 | head -3" 2>&1 | tr -d '\r'
		echo "=== память и разделы ==="
		MSYS_NO_PATHCONV=1 "$ADB" shell "head -2 /proc/meminfo; cat /proc/iomem 2>/dev/null | head -6" 2>&1 | tr -d '\r'
		echo ГОТОВО
		exit 0
	fi
	sleep 5
done
echo "устройство не появилось"
