#!/bin/bash
S=$HOME/leo-pmos/arch/arm/mach-msm/qdsp6_1550/q6audio.c
L=$(grep -n 'int q6audio_set_rx_volume' "$S" | cut -d: -f1)
sed -n "$((L)),$((L+55))p" "$S"
