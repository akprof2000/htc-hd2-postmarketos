#!/bin/bash
P=$HOME/.local/var/pmbootstrap/packages
echo "=== архитектуры в кэше пакетов ==="
ls $P/edge 2>/dev/null
echo "=== пакеты gcc4 в кэше ==="
find $P -name 'gcc4*' -o -name '*binutils*' 2>/dev/null | head -10
echo "=== всего пакетов по архитектурам ==="
for d in $P/edge/*/; do echo "$d: $(ls "$d"/*.apk 2>/dev/null | wc -l) шт"; done
echo "=== версия pmbootstrap ==="
$HOME/.local/bin/pmbootstrap --version 2>/dev/null
