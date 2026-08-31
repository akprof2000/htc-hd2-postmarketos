#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap --offline -y build --arch armv7 --force linux-htc-leo > /tmp/build.log 2>&1
echo "код: $?"
grep -E "03-gpio|Kernel:|ERROR" /tmp/build.log | tail -6
tail -3 /tmp/build.log
K=$(ls -t $HOME/.local/var/pmbootstrap/packages/edge/armv7/linux-htc-leo-*.apk 2>/dev/null | head -1)
echo "пакет: $K"
if [ -n "$K" ]; then
	rm -rf /tmp/kk; mkdir /tmp/kk; tar xzf "$K" -C /tmp/kk 2>/dev/null
	ls -l /tmp/kk/boot/ 2>/dev/null
	cp /tmp/kk/boot/vmlinuz* /mnt/c/Projects/HTC-HD2-T8585/out/zImage-gpiofix 2>/dev/null
	ls -l /mnt/c/Projects/HTC-HD2-T8585/out/zImage-gpiofix
fi
