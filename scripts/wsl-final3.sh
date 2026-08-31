#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
# убрать все прежние 06/07, поставить новую пару
sed -i '/06-.*\.patch/d; /07-.*\.patch/d' "$A/APKBUILD"
rm -f "$A"/06-*.patch "$A"/07-*.patch
cp /mnt/c/Projects/HTC-HD2-T8585/out/06-fb-standard-rgb.patch "$A/"
cp /mnt/c/Projects/HTC-HD2-T8585/out/07-mdp-rgb-pack.patch "$A/"
awk '/^\t05-drop-missing-msm_kexec\.patch$/ { print; print "\t06-fb-standard-rgb.patch"; print "\t07-mdp-rgb-pack.patch"; next } { print }' "$A/APKBUILD" > "$A/APKBUILD.new" && mv "$A/APKBUILD.new" "$A/APKBUILD"
grep -E '0[567]-' "$A/APKBUILD"
pmbootstrap checksum linux-htc-leo >/dev/null 2>&1
pmbootstrap --offline -y build --arch armv7 --force linux-htc-leo > $HOME/b.log 2>&1
echo "код: $?"
grep -E 'Done|error:|No rule|malformed' $HOME/b.log | tail -1
K=$(ls -t $HOME/.local/var/pmbootstrap/packages/edge/armv7/linux-htc-leo-*.apk | head -1)
rm -rf /tmp/kk; mkdir /tmp/kk; tar xzf "$K" -C /tmp/kk 2>/dev/null
cp /tmp/kk/boot/vmlinuz* /mnt/c/Projects/HTC-HD2-T8585/out/zImage-final
md5sum /mnt/c/Projects/HTC-HD2-T8585/out/zImage-final
