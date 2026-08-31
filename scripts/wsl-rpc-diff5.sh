#!/bin/bash
A=$HOME/leo232/arch/arm/mach-msm
B=$HOME/leo-pmos/arch/arm/mach-msm
echo '=== как определяется nand_boot в 3.0 ==='
sed -n '95,130p' $B/board-htcleo.c
echo '=== 2.6.32: третье место (1290-1320) ==='
sed -n '1290,1320p' $A/smd_rpcrouter.c
echo '=== есть ли аналог в 3.0 (rpcrouter_init / smd probe) ==='
grep -n 'is_nand_boot' $B/smd_rpcrouter.c
