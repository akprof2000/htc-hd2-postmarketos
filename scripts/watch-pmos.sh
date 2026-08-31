#!/bin/sh
# Ждёт, пока pmOS появится в сети, и заходит по SSH.
# Различаем системы по имени в DHCP: htc-leo = pmOS, HTC-HD2 = Windows Mobile,
# android-... = Android ICS. MAC у всех трёх одинаковый.
cd /c/Projects/HTC-HD2-T8585 || exit 1
R="ssh -i out/ssh/openwrt_key -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o ConnectTimeout=8 root@192.168.100.1"

i=0
while [ $i -lt 480 ]; do
	i=$((i + 1))
	LEASE=$(MSYS_NO_PATHCONV=1 $R "grep -i '7c:61:93:37:3c:26' /tmp/dhcp.leases 2>/dev/null" 2>/dev/null)
	NAME=$(echo "$LEASE" | awk '{print $4}')
	IP=$(echo "$LEASE" | awk '{print $3}')

	if [ -n "$IP" ]; then
		# кто именно в эфире прямо сейчас
		ALIVE=$(MSYS_NO_PATHCONV=1 $R "ping -c 1 -W 2 $IP >/dev/null 2>&1 && echo да" 2>/dev/null)
		if [ "$ALIVE" = "да" ]; then
			SSH_OK=$(MSYS_NO_PATHCONV=1 $R "nc -w 3 -z $IP 22 2>/dev/null && echo открыт" 2>/dev/null)
			echo "$(date '+%H:%M:%S') $IP имя=$NAME в_эфире=да ssh=${SSH_OK:-закрыт}"
			if [ "$SSH_OK" = "открыт" ]; then
				echo "=== SSH ОТКРЫТ на $IP ==="
				exit 0
			fi
		else
			[ $((i % 6)) -eq 0 ] && echo "$(date '+%H:%M:%S') аренда $IP имя=$NAME, но не отвечает"
		fi
	else
		[ $((i % 6)) -eq 0 ] && echo "$(date '+%H:%M:%S') аренды нет"
	fi
	sleep 15
done
echo "за два часа pmOS в сети не появилась"
