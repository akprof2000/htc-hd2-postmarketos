#!/bin/bash
# Патч 04: WinCE-kick для RPC-роутера. Добавляет в APKBUILD и собирает.
set -u
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
P=04-rpcrouter-wince-kick.patch
cp "/mnt/c/Projects/HTC-HD2-T8585/out/$P" "$A/$P"
if ! grep -q "$P" "$A/APKBUILD"; then
	awk -v p="$P" '
		/^\t03-gpio-clear-before-handler\.patch$/ { print; print "\t" p; next }
		{ print }
	' "$A/APKBUILD" > "$A/APKBUILD.new" && mv "$A/APKBUILD.new" "$A/APKBUILD"
fi
grep -c "04-rpcrouter" "$A/APKBUILD"
pmbootstrap checksum linux-htc-leo >/dev/null 2>&1
for try in 1 2 3; do
	pmbootstrap --offline -y build --arch armv7 --force linux-htc-leo > /tmp/b.log 2>&1 && break
	tail -2 /tmp/b.log; sleep 15
done
grep -E "Done|ERROR" /tmp/b.log | tail -2
K=$(ls -t $HOME/.local/var/pmbootstrap/packages/edge/armv7/linux-htc-leo-*.apk | head -1)
rm -rf /tmp/kk; mkdir /tmp/kk; tar xzf "$K" -C /tmp/kk 2>/dev/null
cp /tmp/kk/boot/vmlinuz* /mnt/c/Projects/HTC-HD2-T8585/out/zImage-rpcfix
md5sum /mnt/c/Projects/HTC-HD2-T8585/out/zImage-rpcfix
