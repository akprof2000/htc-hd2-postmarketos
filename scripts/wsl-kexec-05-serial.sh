#!/bin/bash
# Пересборка в один поток: с -j ошибки тонут среди предупреждений.
set -u
export PATH="$HOME/.local/bin:$PATH"
X="gcc4-armv7-alpine-linux-musleabihf-"
pmbootstrap -y chroot -- sh -c "cd /leo-ics && make ARCH=arm CROSS_COMPILE=$X zImage 2>&1 | grep -E 'error:|Error [0-9]|\*\*\*' | head -30"
