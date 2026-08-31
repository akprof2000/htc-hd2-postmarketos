#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
# убрать патч 06 из APKBUILD и удалить файл
sed -i '/06-panel-bgr-colors.patch/d' "$A/APKBUILD"
rm -f "$A/06-panel-bgr-colors.patch"
grep -c '06-panel' "$A/APKBUILD"
pmbootstrap checksum linux-htc-leo >/dev/null 2>&1
pmbootstrap --offline -y build --arch armv7 --force linux-htc-leo > $HOME/b.log 2>&1
echo "код: $?"
grep -E 'Done|error:|No rule' $HOME/b.log | tail -1
K=$(ls -t $HOME/.local/var/pmbootstrap/packages/edge/armv7/linux-htc-leo-*.apk | head -1)
rm -rf /tmp/kk; mkdir /tmp/kk; tar xzf "$K" -C /tmp/kk 2>/dev/null
cp /tmp/kk/boot/vmlinuz* /mnt/c/Projects/HTC-HD2-T8585/out/zImage-final
md5sum /mnt/c/Projects/HTC-HD2-T8585/out/zImage-final
