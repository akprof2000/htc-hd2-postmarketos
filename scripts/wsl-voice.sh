#!/bin/bash
S=$HOME/leo-pmos/arch/arm/mach-msm/qdsp6_1550
echo "=== номера ioctl голосового тракта ==="
grep -n 'START_VOICE\|STOP_VOICE\|SWITCH_DEVICE\|SET_MUTE' $HOME/leo-pmos/include/linux/msm_audio.h
echo "=== что делает START_VOICE ==="
sed -n '/case AUDIO_START_VOICE/,/break;/p' $S/audio_ctl.c | head -20
echo "=== q6voice функции ==="
grep -n 'q6voice_start\|q6audio_set_tx_mute\|start_voice' $S/q6audio.c | head -6
