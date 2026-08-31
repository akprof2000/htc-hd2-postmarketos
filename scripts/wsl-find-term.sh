set -eu
B=/mnt/c/Projects/HTC-HD2-T8585/backup/sdcard/ICS
m=$(mktemp -d)
sudo mount -o loop,ro "$B/system.ext4" "$m"
echo "=== менеджер рут-прав ==="
sudo ls "$m/app" | grep -iE "super|root|magisk" || echo "в /system/app нет"
echo "=== su ==="
sudo ls -l "$m/xbin/su"
sudo umount "$m"; rmdir "$m"
