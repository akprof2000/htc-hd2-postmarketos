#!/bin/bash
echo "=== чем собирается linux-htc-leo ==="
grep -nE 'makedepends|gcc|CROSS|_carch|HOSTCC' $HOME/pmaports/device/archived/linux-htc-leo/APKBUILD 2>/dev/null | head -15
echo "=== где в pmaports живёт gcc4.9 ==="
find $HOME/pmaports -maxdepth 3 -type d -name 'gcc4*' 2>/dev/null
echo "=== готовые пакеты, собранные ранее ==="
find $HOME/.local/var/pmbootstrap/packages -name '*gcc4*' 2>/dev/null | head
find $HOME/.local/var/pmbootstrap/packages -maxdepth 2 -type d 2>/dev/null | head
