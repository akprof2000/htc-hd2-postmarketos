#!/bin/bash
F=$HOME/leo-pmos/arch/arm/mach-msm/rpcrouter_smd_xprt.c
echo "=== обработчик событий канала ==="
grep -n -A25 'rpcrouter_smd_remote_notify' "$F" | head -32
echo "=== кто зовёт add_xprt ==="
grep -rn 'msm_rpcrouter_xprt_notify\|XPRT_OPEN\|add_xprt' $HOME/leo-pmos/arch/arm/mach-msm/rpcrouter_smd_xprt.c $HOME/leo-pmos/arch/arm/mach-msm/smd_rpcrouter.c | head -8
