#!/bin/bash
S=$HOME/leo-pmos/arch/arm/mach-msm/qdsp6_1550/q6audio.c
sed -n '1530,1585p' "$S"
