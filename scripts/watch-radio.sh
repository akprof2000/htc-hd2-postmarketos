#!/bin/sh
# Следит за эфиром со стороны роутера: любые события по MAC телефона + порт 22.
R="ssh -i out/ssh/openwrt_key -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o ConnectTimeout=8 root@192.168.100.1"
cd /c/Projects/HTC-HD2-T8585 || exit 1
LAST=""
i=0
while [ $i -lt 60 ]; do
	i=$((i + 1))
	EV=$(MSYS_NO_PATHCONV=1 $R "logread 2>/dev/null | grep -i '7c:61:93:37:3c:26' | tail -1" 2>/dev/null)
	if [ -n "$EV" ] && [ "$EV" != "$LAST" ]; then
		LAST="$EV"
		echo "$EV" | sed 's/Sun Aug 30 //; s/ 2026//'
	fi
	if MSYS_NO_PATHCONV=1 $R "nc -w 2 -z 192.168.100.235 22 2>/dev/null" 2>/dev/null; then
		echo "=== SSH ОТКРЫТ ==="
		exit 0
	fi
	sleep 15
done
echo "за 15 минут SSH не открылся"
