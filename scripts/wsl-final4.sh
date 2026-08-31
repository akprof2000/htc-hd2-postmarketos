#!/bin/bash
# Ядро: + патч 08 (очистка «зебры») + драйвер сброса модема
set -u
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
C=$(ls $A/config-* | head -1)

cp /mnt/c/Projects/HTC-HD2-T8585/out/08-fb-clear-boot-garbage.patch "$A/"
grep -q '08-fb-clear' "$A/APKBUILD" || awk '/^\t07-mdp-rgb-pack\.patch$/ { print; print "\t08-fb-clear-boot-garbage.patch"; next } { print }' "$A/APKBUILD" > "$A/APKBUILD.new" && mv "$A/APKBUILD.new" "$A/APKBUILD"

# драйвер сброса модема — встроенным, не модулем
sed -i '/CONFIG_MSM_RESET_MODEM/d' "$C"
echo 'CONFIG_MSM_RESET_MODEM=y' >> "$C"
grep -E 'RESET_MODEM' "$C"
grep -E '0[78]-' "$A/APKBUILD"

pmbootstrap checksum linux-htc-leo >/dev/null 2>&1
pmbootstrap --offline -y build --arch armv7 --force linux-htc-leo > $HOME/b.log 2>&1
echo "код: $?"
grep -E 'Done|error:|No rule|malformed' $HOME/b.log | tail -1
K=$(ls -t $HOME/.local/var/pmbootstrap/packages/edge/armv7/linux-htc-leo-*.apk | head -1)
rm -rf /tmp/kk; mkdir /tmp/kk; tar xzf "$K" -C /tmp/kk 2>/dev/null
cp /tmp/kk/boot/vmlinuz* /mnt/c/Projects/HTC-HD2-T8585/out/zImage-final
md5sum /mnt/c/Projects/HTC-HD2-T8585/out/zImage-final
