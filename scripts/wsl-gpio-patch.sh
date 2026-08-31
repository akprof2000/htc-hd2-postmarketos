#!/bin/bash
# Проверяем контекст патча по реальному дереву и структуру regs (есть ли int_clear)
F=$HOME/leo-pmos/arch/arm/mach-msm/gpio.c
grep -n "int_clear" $HOME/leo-pmos/arch/arm/mach-msm/gpio_hw.h 2>/dev/null | head -3
grep -rn "int_clear" $HOME/leo-pmos/arch/arm/mach-msm/gpio.c | head -3
sed -n '503,513p' "$F"
