#!/bin/sh
# Собрать всё, что нужно для запуска, в отдельную папку.
cd /home/rockbox/build || exit 1
rm -rf /home/rbstage
make install prefix=/usr/local DESTDIR=/home/rbstage >/home/rbinstall.log 2>&1
echo "код: $?"
tail -4 /home/rbinstall.log
echo "=== что получилось:"
find /home/rbstage -maxdepth 4 -type d 2>/dev/null | head -12
du -sh /home/rbstage 2>/dev/null
