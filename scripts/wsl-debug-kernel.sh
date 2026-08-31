#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
cp /mnt/c/Projects/HTC-HD2-T8585/out/04-rpcrouter-wince-kick.patch "$A/"
C=$(ls $A/config-* | head -1)
grep -q 'CONFIG_MSM_ONCRPCROUTER_DEBUG=y' "$C" || {
	sed -i '/CONFIG_MSM_ONCRPCROUTER_DEBUG/d' "$C"
	echo 'CONFIG_MSM_ONCRPCROUTER_DEBUG=y' >> "$C"
}
grep ONCRPCROUTER_DEBUG "$C"
pmbootstrap checksum linux-htc-leo >/dev/null 2>&1
pmbootstrap --offline -y build --arch armv7 --force linux-htc-leo > $HOME/b.log 2>&1
echo "код: $?"
grep -E 'error:|No rule|Done' $HOME/b.log | tail -2
K=$(ls -t $HOME/.local/var/pmbootstrap/packages/edge/armv7/linux-htc-leo-*.apk | head -1)
rm -rf /tmp/kk; mkdir /tmp/kk; tar xzf "$K" -C /tmp/kk 2>/dev/null
cp /tmp/kk/boot/vmlinuz* /mnt/c/Projects/HTC-HD2-T8585/out/zImage-rpcdebug
md5sum /mnt/c/Projects/HTC-HD2-T8585/out/zImage-rpcdebug
