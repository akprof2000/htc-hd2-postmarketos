#!/bin/bash
A=$HOME/leo232/arch/arm/mach-msm     # 2.6.32: звук под HaRET работает
B=$HOME/leo-pmos/arch/arm/mach-msm   # 3.0 LRXS: сервисы не приходят
echo '=== версии протокола ==='
grep -n 'define RPCROUTER_VERSION' $A/smd_rpcrouter.h $B/smd_rpcrouter.h
echo '=== структура HELLO/управляющих сообщений ==='
grep -n -A3 'struct rr_control_msg' $A/smd_rpcrouter.h | head -8
echo '---'
grep -n -A3 'struct rr_control_msg' $B/smd_rpcrouter.h | head -12
echo '=== объём diff по .c ==='
diff $A/smd_rpcrouter.c $B/smd_rpcrouter.c | wc -l
echo '=== процесс HELLO в 2.6.32 ==='
grep -n -B2 -A12 'case RPCROUTER_CTRL_CMD_HELLO' $A/smd_rpcrouter.c | head -22
