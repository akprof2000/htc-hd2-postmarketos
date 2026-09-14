#!/bin/sh
# Положить новое ядро в \PMOS\ на карте памяти прямо из работающей pmOS —
# без Android и кабеля. NAND не трогается: HaRET грузит ядро с карты.
#
#   sh scripts/install-kernel-sd.sh out/zImage-btsco
#
# Текущее ядро сохраняется как zImage.prev-<дата> — откат:
#   mount -o rw -t vfat /dev/mmcblk0p1 /mnt/sd
#   cp /mnt/sd/PMOS/zImage.prev-<дата> /mnt/sd/PMOS/zImage
# либо тем же скриптом с сохранённым файлом.
#
# Новое ядро начнёт работать после перезагрузки через \PMOS\haret.exe.
set -eu
NEW="${1:?укажите файл ядра, например out/zImage-btsco}"
[ -f "$NEW" ] || { echo "нет файла $NEW"; exit 1; }
KEY=out/ssh/openwrt_key
HOST=root@192.168.100.235
SSH="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -i $KEY $HOST"
STAMP=$(date +%m%d%H%M)
SUM=$(md5sum "$NEW" | cut -d' ' -f1)

echo "новое ядро: $NEW ($SUM)"

# 1. раздел карты — на запись, текущее ядро — в сохранёнку
$SSH "set -e
mkdir -p /mnt/sd
mountpoint -q /mnt/sd && umount /mnt/sd
mount -o rw -t vfat /dev/mmcblk0p1 /mnt/sd
cp /mnt/sd/PMOS/zImage /mnt/sd/PMOS/zImage.prev-$STAMP
echo \"сохранено текущее ядро: zImage.prev-$STAMP (\$(md5sum /mnt/sd/PMOS/zImage.prev-$STAMP | cut -d' ' -f1))\""

# 2. новое ядро — во временный файл, затем на место
$SSH "cat > /mnt/sd/PMOS/zImage.new" < "$NEW"
$SSH "set -e
got=\$(md5sum /mnt/sd/PMOS/zImage.new | cut -d' ' -f1)
if [ \"\$got\" != \"$SUM\" ]; then
  echo \"КОНТРОЛЬНАЯ СУММА НЕ СОШЛАСЬ (\$got) — текущее ядро не тронуто\"
  rm -f /mnt/sd/PMOS/zImage.new
  sync; umount /mnt/sd; exit 1
fi
mv /mnt/sd/PMOS/zImage.new /mnt/sd/PMOS/zImage
sync
echo \"на карте: \$(md5sum /mnt/sd/PMOS/zImage | cut -d' ' -f1)\"
ls -la /mnt/sd/PMOS/
umount /mnt/sd
echo 'раздел карты отмонтирован — можно перезагружаться через \\PMOS\\haret.exe'"
