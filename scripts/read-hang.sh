#!/bin/sh
# Забирает журнал зависшей загрузки pmOS из ram_console (initcall_debug покажет
# последний начатый вызов инициализации - место зависания).
ADB="/c/Users/akpro/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"
cd /c/Projects/HTC-HD2-T8585 || exit 1
i=0
while [ $i -lt 720 ]; do
	i=$((i + 1))
	if [ "$(MSYS_NO_PATHCONV=1 "$ADB" devices | grep -c 'device$')" -gt 0 ]; then
		sleep 3
		MSYS_NO_PATHCONV=1 "$ADB" shell "cat /proc/last_kmsg" > logs/last_kmsg-hang.txt 2>&1
		tr -d '\r' < logs/last_kmsg-hang.txt > logs/hang.txt
		echo "строк: $(wc -l < logs/hang.txt)"
		grep -a "Linux version" logs/hang.txt | head -1
		echo "--- последние 15 строк ---"
		tail -15 logs/hang.txt
		echo КОНЕЦ
		exit 0
	fi
	sleep 5
done
echo "устройство не появилось"
