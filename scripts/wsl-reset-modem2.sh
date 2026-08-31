#!/bin/bash
F=$HOME/leo-pmos/arch/arm/mach-msm/reset_modem.c
echo "=== интерфейс драйвера ==="
grep -n 'misc\|\.name\|write\|open\|module_init\|MODULE' "$F" | head -12
echo "=== что делают команды ==="
sed -n '55,95p' "$F"
echo "=== включён ли в сборку ==="
grep -n 'reset_modem' $HOME/leo-pmos/arch/arm/mach-msm/Makefile
