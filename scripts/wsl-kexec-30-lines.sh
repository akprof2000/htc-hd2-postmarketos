#!/bin/bash
F=$HOME/kexec-tools-2.0.20/kexec/arch/arm/kexec-zImage-arm.c
echo "=== строки 640..665 ==="
sed -n '640,665p' "$F"
echo "=== что такое extra_size ==="
grep -n 'extra_size' "$F" | head -8
