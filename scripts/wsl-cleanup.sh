set +e
for m in $(mount | awk '/tmp.tmp\./{print $3}'); do sudo umount "$m" && rmdir "$m"; done
for l in $(losetup -a | awk -F: '/htc-leo-sdcard/{print $1}'); do sudo losetup -d "$l"; done
echo "--- остатки ---"
mount | grep -c 'tmp\.tmp\.'
losetup -a | grep -c htc-leo-sdcard
