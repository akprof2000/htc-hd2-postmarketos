#!/bin/bash
# Повтор сборки pmOS-ядра с CONFIG_KEXEC, полный лог ошибки компиляции.
set -u
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
C=$(ls $A/config-* | head -1)
grep -E '^CONFIG_KEXEC|^CONFIG_ATAGS_PROC' "$C"
pmbootstrap --offline -y build --arch armv7 --force linux-htc-leo > /tmp/b.log 2>&1
echo "код: $?"
grep -E 'error:|Error [0-9]' /tmp/b.log | head -10
