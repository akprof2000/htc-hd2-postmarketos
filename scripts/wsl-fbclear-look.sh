#!/bin/bash
F=$HOME/leo-pmos/drivers/video/msm/msm_fb.c
grep -n 'setup_fbmem\|smem_start\|smem_len\|ioremap\|fb->screen_base\|register_framebuffer' "$F" | head -12
echo "--- контекст вокруг register_framebuffer ---"
L=$(grep -n 'register_framebuffer' "$F" | head -1 | cut -d: -f1)
sed -n "$((L-14)),$((L+3))p" "$F"
