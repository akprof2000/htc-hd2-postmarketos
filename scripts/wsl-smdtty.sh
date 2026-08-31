#!/bin/bash
A=$HOME/pmaports/device/archived/linux-htc-leo
C=$(ls $A/config-* | head -1)
echo "=== smd_tty / rmnet / модемные опции ==="
grep -E 'SMD_TTY|MSM_RMNET|MSM_SMD$|MSM_SMD=|ONCRPC|MSM_DATAMOVER' "$C" | head -8
echo "=== есть ли драйвер в дереве ==="
ls $HOME/leo-pmos/arch/arm/mach-msm/smd_tty.c 2>/dev/null && echo "smd_tty.c есть"
grep -n 'SMD_TTY' $HOME/leo-pmos/arch/arm/mach-msm/Kconfig | head -4
echo "=== какие каналы он открывает ==="
grep -n 'smd_tty_channels\|"DS"\|"GPS"\|"APPS"' $HOME/leo-pmos/arch/arm/mach-msm/smd_tty.c 2>/dev/null | head -8
