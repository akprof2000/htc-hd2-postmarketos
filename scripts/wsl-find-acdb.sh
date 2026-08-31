#!/bin/bash
M=/tmp/icsys
sudo mkdir -p $M
sudo mount -o loop,ro /mnt/c/Projects/HTC-HD2-T8585/backup/sdcard/ICS/system.ext4 $M 2>/dev/null
find $M -iname '*acdb*' 2>/dev/null
find $M -iname 'AudioPara*' -o -iname '*audio*.csv' 2>/dev/null | head -5
ls $M/etc/ | grep -iE 'audio|acdb|snd' | head -8
sudo umount $M
