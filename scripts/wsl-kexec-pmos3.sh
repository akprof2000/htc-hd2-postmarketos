#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
P=05-drop-missing-msm_kexec.patch
cp "/mnt/c/Projects/HTC-HD2-T8585/out/$P" "$A/$P"
grep -q "$P" "$A/APKBUILD" || awk -v p="$P" '/^\t04-rpcrouter-wince-kick\.patch$/ { print; print "\t" p; next } { print }' "$A/APKBUILD" > "$A/APKBUILD.new" && mv "$A/APKBUILD.new" "$A/APKBUILD"
pmbootstrap checksum linux-htc-leo >/dev/null 2>&1
pmbootstrap --offline -y build --arch armv7 --force linux-htc-leo > $HOME/b.log 2>&1
echo "код: $?"
grep -E 'error:|No rule|Done' $HOME/b.log | tail -3
K=$(ls -t $HOME/.local/var/pmbootstrap/packages/edge/armv7/linux-htc-leo-*.apk | head -1)
rm -rf /tmp/kk; mkdir /tmp/kk; tar xzf "$K" -C /tmp/kk 2>/dev/null
cp /tmp/kk/boot/vmlinuz* /mnt/c/Projects/HTC-HD2-T8585/out/zImage-kexec-pmos 2>/dev/null
md5sum /mnt/c/Projects/HTC-HD2-T8585/out/zImage-kexec-pmos 2>/dev/null
