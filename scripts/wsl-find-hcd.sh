#!/bin/bash
# Ищем прошивку BT (.hcd) в system.ext4 из резервной копии ICS
M=/tmp/icsys
sudo mkdir -p $M
sudo mount -o loop,ro /mnt/c/Projects/HTC-HD2-T8585/backup/sdcard/ICS/system.ext4 $M 2>/dev/null || { echo "mount fail"; exit 1; }
find $M -iname '*.hcd' 2>/dev/null
find $M -iname '*bcm*' 2>/dev/null | head -5
H=$(find $M -iname '*.hcd' | head -1)
if [ -n "$H" ]; then
	cp "$H" /mnt/c/Projects/HTC-HD2-T8585/out/bcm4329.hcd
	ls -l /mnt/c/Projects/HTC-HD2-T8585/out/bcm4329.hcd
fi
sudo umount $M
