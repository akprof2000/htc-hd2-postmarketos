#!/bin/bash
S=$HOME/leo-pmos/arch/arm/mach-msm/qdsp6_1550/q6audio.c
echo "=== _audio_rx_path_enable (1671..1700) ==="
sed -n '1671,1700p' "$S"
echo "=== _audio_rx_path_disable (1768..1790) ==="
sed -n '1768,1790p' "$S"
