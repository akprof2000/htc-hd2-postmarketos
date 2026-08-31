#!/bin/bash
# Гипотеза: musl иначе трактует %Lx (в glibc это unsigned long long).
# Тогда разбор /proc/iomem не срабатывает и список памяти пуст.
set -u
echo "=== как написан разбор карты памяти ==="
grep -n 'sscanf' $HOME/kexec-tools-2.0.20/kexec/arch/arm/kexec-arm.c 2>/dev/null
grep -n 'sscanf' $HOME/kexec-tools-2.0.20/kexec/kexec.c 2>/dev/null | head -5
echo "=== типы переменных ==="
grep -n -B3 'sscanf' $HOME/kexec-tools-2.0.20/kexec/arch/arm/kexec-arm.c 2>/dev/null | head -12
