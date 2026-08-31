#!/bin/bash
S=$HOME/leo-pmos
sed -n '130,180p' $S/arch/arm/mach-msm/qdsp6_1550/audio_ctl.c
echo '=== устройства (device id) ==='
grep -rn 'ADSP_AUDIO_DEVICE_ID\|CAD_HW_DEVICE_ID\|device_id' $S/arch/arm/mach-msm/qdsp6_1550/q6audio.c | head -8
grep -rn 'SPKR\|SPEAKER' $S/arch/arm/mach-msm/include/mach/qdsp6_1550/*.h 2>/dev/null | head -8
