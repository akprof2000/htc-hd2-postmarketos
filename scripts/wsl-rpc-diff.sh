#!/bin/bash
A=$HOME/leo-ics/arch/arm/mach-msm      # 2.6.32 - звук под HaRET работает
B=$HOME/leo-pmos/arch/arm/mach-msm    # 3.0 LRXS - сервисы не регистрируются
ls $A/smd_rpcrouter.c $B/smd_rpcrouter.c 2>/dev/null
echo '=== версии протокола ==='
grep -n 'RPCROUTER_VERSION\|R2R_VERSION\|VERSION' $A/smd_rpcrouter.h | head -5
grep -n 'RPCROUTER_VERSION\|R2R_VERSION\|VERSION' $B/smd_rpcrouter.h | head -5
echo '=== размер diff ==='
diff $A/smd_rpcrouter.c $B/smd_rpcrouter.c | wc -l
echo '=== HELLO/handshake в обоих ==='
grep -n 'HELLO\|hello' $A/smd_rpcrouter.c | head -6
echo '---'
grep -n 'HELLO\|hello' $B/smd_rpcrouter.c | head -6
