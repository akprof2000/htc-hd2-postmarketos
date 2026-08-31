#!/bin/bash
# Распаковываем дерево pmOS-ядра из кеша и смотрим msm_init_gpio
cd $HOME || exit 1
if [ ! -d leo-pmos ]; then
	mkdir leo-pmos
	tar xzf $HOME/.local/var/pmbootstrap/cache_distfiles/linux-htc-leo-fb2ba086ea96647f38539664ebf0aa6eca61d7bb.tar.gz -C leo-pmos --strip-components=1
fi
F=$(ls leo-pmos/arch/arm/mach-msm/gpio.c 2>/dev/null)
echo "файл: $F"
grep -n "msm_init_gpio" "$F" | head -3
L=$(grep -n "static int __init msm_init_gpio" "$F" | cut -d: -f1)
sed -n "$((L-4)),$((L+55))p" "$F"
