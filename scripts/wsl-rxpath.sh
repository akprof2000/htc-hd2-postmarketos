#!/bin/bash
S=$HOME/leo-pmos/arch/arm/mach-msm/qdsp6_1550/q6audio.c
echo "=== кто зовёт speaker_enable ==="
grep -n 'speaker_enable\|analog_ops->' "$S" | head -12
echo "=== audio_rx_path_enable / disable ==="
sed -n '/static void audio_rx_path_enable/,/^}/p' "$S" | head -25
