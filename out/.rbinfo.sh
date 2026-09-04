#!/bin/sh
echo "=== все NEEDED:"
readelf -d /home/rockbox/build/rockbox 2>/dev/null | grep -c NEEDED
readelf -d /home/rockbox/build/rockbox 2>/dev/null | grep NEEDED
echo "=== содержимое сборки:"
ls /home/rockbox/build | head -20
echo "=== есть ли .rockbox:"
ls -d /home/rockbox/build/.rockbox 2>/dev/null && du -sh /home/rockbox/build/.rockbox
echo "=== цели установки:"
grep -nE '^install|^fullinstall' /home/rockbox/build/Makefile | head -5
