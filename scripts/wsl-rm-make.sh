#!/bin/bash
grep -n 'reset_modem' $HOME/leo-pmos/arch/arm/mach-msm/Makefile
grep -n -A4 'MSM_RESET_MODEM\|config.*RESET' $HOME/leo-pmos/arch/arm/mach-msm/Kconfig | head -8
grep -E 'RESET_MODEM' $HOME/pmaports/device/archived/linux-htc-leo/config-* 
