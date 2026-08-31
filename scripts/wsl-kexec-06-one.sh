#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
X="gcc4-armv7-alpine-linux-musleabihf-"
pmbootstrap -y chroot -- sh -c "cd /leo-ics && make ARCH=arm CROSS_COMPILE=$X arch/arm/mm/proc-v7.o 2>&1 | tail -25"
