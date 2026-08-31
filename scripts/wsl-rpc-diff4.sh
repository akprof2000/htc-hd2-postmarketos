#!/bin/bash
A=$HOME/leo232/arch/arm/mach-msm
B=$HOME/leo-pmos/arch/arm/mach-msm
echo '=== все места с nand_boot в rpc/smd 2.6.32 ==='
grep -rn 'is_nand_boot' $A/smd_rpcrouter.c $A/smd.c $A/proc_comm.c 2>/dev/null | head -10
echo '=== есть ли is_nand_boot в 3.0 ==='
grep -rn 'is_nand_boot' $B/*.c $B/include/mach/*.h 2>/dev/null | head -6
echo '=== HELLO-обработчик 3.0 (строка ~1011) ==='
sed -n '1005,1035p' $B/smd_rpcrouter.c
