#!/bin/bash
S=$HOME/leo-pmos
grep -rln "msm_audio_ctl" $S/arch/arm/mach-msm --include=*.c | head -3
F=$(grep -rln "msm_audio_ctl" $S/arch/arm/mach-msm --include=*.c | head -1)
echo "== $F =="
grep -nE 'SWITCH_DEVICE|SET_VOLUME|ioctl|device_id|htc_' "$F" | head -20
grep -rn 'AUDIO_SWITCH_DEVICE' $S/include/linux/msm_audio.h
