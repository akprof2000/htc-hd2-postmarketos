#!/bin/bash
# Финальное ядро: патчи 01-06, KEXEC, без ONCRPCROUTER_DEBUG.
set -u
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
cp /mnt/c/Projects/HTC-HD2-T8585/out/06-panel-bgr-colors.patch "$A/"
grep -q '06-panel-bgr-colors' "$A/APKBUILD" || awk '/^\t05-drop-missing-msm_kexec\.patch$/ { print; print "\t06-panel-bgr-colors.patch"; next } { print }' "$A/APKBUILD" > "$A/APKBUILD.new" && mv "$A/APKBUILD.new" "$A/APKBUILD"
C=$(ls $A/config-* | head -1)
sed -i '/CONFIG_MSM_ONCRPCROUTER_DEBUG/d' "$C"
echo '# CONFIG_MSM_ONCRPCROUTER_DEBUG is not set' >> "$C"
pmbootstrap checksum linux-htc-leo >/dev/null 2>&1
pmbootstrap --offline -y build --arch armv7 --force linux-htc-leo > $HOME/b.log 2>&1
echo "код сборки: $?"
grep -E 'Done|malformed|No rule|error:' $HOME/b.log | tail -2
K=$(ls -t $HOME/.local/var/pmbootstrap/packages/edge/armv7/linux-htc-leo-*.apk | head -1)
rm -rf /tmp/kk; mkdir /tmp/kk; tar xzf "$K" -C /tmp/kk 2>/dev/null
cp /tmp/kk/boot/vmlinuz* /mnt/c/Projects/HTC-HD2-T8585/out/zImage-final
md5sum /mnt/c/Projects/HTC-HD2-T8585/out/zImage-final
