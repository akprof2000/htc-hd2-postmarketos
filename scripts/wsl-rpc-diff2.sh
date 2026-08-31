#!/bin/bash
A=$HOME/.local/var/pmbootstrap/chroot_native/leo-ics/arch/arm/mach-msm
B=$HOME/leo-pmos/arch/arm/mach-msm
sudo ls $A/smd_rpcrouter.c >/dev/null && echo "ICS-дерево: есть"
echo '=== версия протокола ==='
sudo grep -n 'RPCROUTER_VERSION' $A/smd_rpcrouter.h | head -2
grep -n 'RPCROUTER_VERSION' $B/smd_rpcrouter.h | head -2
echo '=== объём различий ==='
sudo diff $A/smd_rpcrouter.c $B/smd_rpcrouter.c | wc -l
echo '=== как ICS обрабатывает HELLO ==='
sudo grep -n 'HELLO' $A/smd_rpcrouter.c | head -8
