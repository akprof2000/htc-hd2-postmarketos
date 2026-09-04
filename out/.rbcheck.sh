#!/bin/sh
F=/home/rockbox/firmware/target/hosted/sdl/pcm-sdl.c
echo "правка в исходнике: $(grep -c msm_pcm_out $F) совпадений"
echo "правка в программе: $(strings /home/rockbox/build/rockbox 2>/dev/null | grep -c msm_pcm_out) совпадений"
ls -la /home/rockbox/build/rockbox
