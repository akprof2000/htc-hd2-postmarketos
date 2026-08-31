set -eu
B=/mnt/c/Projects/HTC-HD2-T8585/backup/sdcard/ICS
m=$(mktemp -d)
sudo mount -o loop,ro "$B/system.ext4" "$m" 2>/dev/null || { echo "не смонтировалось"; exit 1; }
echo "=== версия модулей ICS-ядра ==="
sudo ls "$m/lib/modules" 2>/dev/null || sudo find "$m" -maxdepth 3 -name "*.ko" -printf "%f\n" 2>/dev/null | head
echo "=== версия из модуля ==="
k=$(sudo find "$m" -name "*.ko" 2>/dev/null | head -1)
[ -n "$k" ] && sudo modinfo "$k" 2>/dev/null | grep -E "^(vermagic|name)" || echo "модулей нет"
echo "=== build.prop ==="
sudo grep -E "ro.build.description|ro.product.device" "$m/build.prop" 2>/dev/null | head -3
sudo umount "$m"; rmdir "$m"
