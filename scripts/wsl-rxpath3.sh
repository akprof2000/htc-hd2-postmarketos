#!/bin/bash
S=$HOME/leo-pmos/arch/arm/mach-msm/qdsp6_1550/q6audio.c
grep -n 'audio_rx_path_enable\|adie_enable\|adie_disable\|audio_update_acdb\|_audio_rx_path' "$S" | head -12
echo "--- тело audio_rx_path_enable ---"
sed -n '/^static void audio_rx_path_enable/,/^}/p' "$S" | head -30
