#!/bin/bash
echo "=== есть ли kexec-tools в pmaports ==="
find $HOME/pmaports -maxdepth 3 -type d -name 'kexec*' 2>/dev/null
echo "=== есть ли пакет в репозитории Alpine/pmOS для armv7 ==="
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap -y chroot -r -- apk search -x kexec-tools 2>&1 | tail -3
echo "=== что за система в rootfs-chroot ==="
pmbootstrap -y chroot -r -- sh -c 'uname -m; cat /etc/alpine-release 2>/dev/null' 2>&1 | tail -3
