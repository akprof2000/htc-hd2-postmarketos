#!/bin/bash
S=$HOME/leo-pmos/arch/arm/mach-msm
echo "=== q6audio_set_rx_volume ==="
sed -n '/int q6audio_set_rx_volume/,/^}/p' $S/qdsp6_1550/q6audio.c | head -22
echo "=== htcleo_get_rx_vol ==="
grep -rn -A12 'htcleo_get_rx_vol' $S/board-htcleo-audio.c | head -18
