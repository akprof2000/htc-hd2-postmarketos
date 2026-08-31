#!/bin/sh
set -eu
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap -y chroot -r -- apk add iw 2>&1 | tail -3
R="$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo"
O=/mnt/c/Projects/HTC-HD2-T8585/out/iw
rm -rf "$O"; mkdir -p "$O/usr/sbin" "$O/usr/lib"
sudo cp "$R/usr/sbin/iw" "$O/usr/sbin/"
for f in $(sudo find "$R/usr/lib" -maxdepth 1 -name "libnl*"); do sudo cp -P "$f" "$O/usr/lib/"; done
sudo chown -R 1000:1000 "$O"
echo "=== собрано ==="
find "$O" -type f -o -type l | sed "s|$O||"
