#!/bin/bash
A=$HOME/pmaports/device/archived/linux-htc-leo
C=$(ls $A/config-* | head -1)
echo "=== логотип в конфиге ==="
grep -E 'CONFIG_LOGO|CONFIG_FB_CON|FRAMEBUFFER_CONSOLE' "$C" | head -6
echo "=== штатные логотипы в дереве ==="
ls $HOME/leo-pmos/drivers/video/logo/*.ppm 2>/dev/null | head -4
echo "=== где msmfb выделяет память под экран ==="
grep -n 'fb_mem\|memset\|alloc.*fb\|framebuffer_alloc\|smem' $HOME/leo-pmos/drivers/video/msm/msm_fb.c | head -10
