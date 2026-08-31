#!/bin/bash
# Включает CONFIG_KEXEC в ядре pmOS (для перезагрузок pmOS->pmOS без HaRET).
set -u
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
C=$(ls $A/config-* | head -1)
echo "конфиг: $C"
grep -E 'CONFIG_KEXEC|ATAGS_PROC' "$C" || true
grep -q '^CONFIG_KEXEC=y' "$C" || printf 'CONFIG_KEXEC=y\nCONFIG_ATAGS_PROC=y\n' >> "$C"
sed -i '/# CONFIG_KEXEC is not set/d; /# CONFIG_ATAGS_PROC is not set/d' "$C"
grep -E '^CONFIG_KEXEC|^CONFIG_ATAGS_PROC' "$C"
pmbootstrap checksum linux-htc-leo >/dev/null 2>&1
for try in 1 2 3; do
	pmbootstrap --offline -y build --arch armv7 --force linux-htc-leo > /tmp/b.log 2>&1 && break
	tail -2 /tmp/b.log; sleep 15
done
grep -E "Done|ERROR" /tmp/b.log | tail -1
K=$(ls -t $HOME/.local/var/pmbootstrap/packages/edge/armv7/linux-htc-leo-*.apk | head -1)
rm -rf /tmp/kk; mkdir /tmp/kk; tar xzf "$K" -C /tmp/kk 2>/dev/null
cp /tmp/kk/boot/vmlinuz* /mnt/c/Projects/HTC-HD2-T8585/out/zImage-rpcfix
md5sum /mnt/c/Projects/HTC-HD2-T8585/out/zImage-rpcfix
