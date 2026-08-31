#!/bin/bash
S=$HOME/leo-pmos/arch/arm/mach-msm
echo "=== функции сброса модема ==="
grep -rn 'smsm_reset_modem\|SMSM_RESET\|modem_reset\|SMSM_SYSTEM_DOWNLOAD' $S/*.c $S/include/mach/*.h 2>/dev/null | head -10
echo "=== SMSM состояния ==="
grep -n 'SMSM_INIT\|SMSM_SMDINIT\|SMSM_RPCINIT\|SMSM_A2M\|smsm_change_state' $S/smd.c 2>/dev/null | head -8
