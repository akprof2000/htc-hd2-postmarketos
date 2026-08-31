#!/bin/bash
S=$HOME/leo-pmos
# Таблица id -> device в q6audio.c и числовые значения ADSP-устройств
sed -n '55,110p' $S/arch/arm/mach-msm/qdsp6_1550/q6audio.c
grep -rn 'ADSP_AUDIO_DEVICE_ID_SPKR_PHONE_MONO\b\|ADSP_AUDIO_DEVICE_ID_SPKR_PHONE_STEREO' $S -r --include=*.h | head -4
