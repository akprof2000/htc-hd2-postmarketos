#!/bin/bash
H=$HOME/leo-pmos/include/linux/msm_audio.h
grep -nE 'AUDIO_IOCTL_MAGIC|AUDIO_START|AUDIO_STOP|AUDIO_SET_CONFIG|AUDIO_GET_CONFIG|struct msm_audio_config' "$H" | head -10
sed -n '/struct msm_audio_config {/,/};/p' "$H"
