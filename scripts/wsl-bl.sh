#!/bin/bash
F=$HOME/leo-pmos/arch/arm/mach-msm/board-htcleo-bl-led.c
ls -l "$F"
grep -n 'brightness\|microp\|offset\|SPI\|write' "$F" | head -18
