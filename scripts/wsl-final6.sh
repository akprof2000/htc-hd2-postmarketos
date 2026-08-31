#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
cp /mnt/c/Projects/HTC-HD2-T8585/out/10-adie-keep-open.patch "$A/"
grep -q '10-adie-keep-open' "$A/APKBUILD" || awk '/^\t09-rpc-force-open-event\.patch$/ { print; print "\t10-adie-keep-open.patch"; next } { print }' "$A/APKBUILD" > "$A/APKBUILD.new" && mv "$A/APKBUILD.new" "$A/APKBUILD"
grep -E '09-|10-' "$A/APKBUILD"
pmbootstrap checksum linux-htc-leo >/dev/null 2>&1
pmbootstrap --offline -y build --arch armv7 --force linux-htc-leo > $HOME/b.log 2>&1
echo "код: $?"
grep -E 'Done|error:|malformed' $HOME/b.log | tail -1
K=$(ls -t $HOME/.local/var/pmbootstrap/packages/edge/armv7/linux-htc-leo-*.apk | head -1)
rm -rf /tmp/kk; mkdir /tmp/kk; tar xzf "$K" -C /tmp/kk 2>/dev/null
cp /tmp/kk/boot/vmlinuz* /mnt/c/Projects/HTC-HD2-T8585/out/zImage-final
md5sum /mnt/c/Projects/HTC-HD2-T8585/out/zImage-final
