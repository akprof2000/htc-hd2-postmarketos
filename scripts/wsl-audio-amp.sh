#!/bin/bash
S=$HOME/leo-pmos
echo '=== do_routing: что происходит при переключении ==='
grep -n "q6audio_do_routing" $S/arch/arm/mach-msm/qdsp6_1550/q6audio.c | head -3
L=$(grep -n "int q6audio_do_routing" $S/arch/arm/mach-msm/qdsp6_1550/q6audio.c | head -1 | cut -d: -f1)
sed -n "${L},$((L+40))p" $S/arch/arm/mach-msm/qdsp6_1550/q6audio.c
echo '=== усилитель/спикер-амп ==='
grep -rn "spk_amp\|speaker_amp\|snd_amp\|amp_on" $S/arch/arm/mach-msm/qdsp6_1550/*.c $S/arch/arm/mach-msm/board-htcleo*.c 2>/dev/null | head -8
