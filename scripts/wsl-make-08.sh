#!/bin/bash
# Патч 08: очистка кадрового буфера при инициализации — убирает «зебру»
# (мусор в неинициализированной видеопамяти, видимый всю раннюю загрузку).
set -eu
F=$HOME/leo-pmos/drivers/video/msm/msm_fb.c
cp "$F" /tmp/fb8_a.c
python3 - <<'PY'
s=open('/tmp/fb8_a.c').read()
old="""	fb->screen_base = fbram;"""
new="""	fb->screen_base = fbram;
	/* Видеопамять после WinMo содержит мусор («зебра»), видимый всю раннюю
	 * загрузку. Гасим её в чёрный сразу, как только буфер отображён. */
	memset(fbram, 0, resource_size);"""
assert old in s, "anchor screen_base"
open('/tmp/fb8_b.c','w').write(s.replace(old,new,1))
print("вставка ок")
PY
cd /tmp
diff -u fb8_a.c fb8_b.c | sed 's|^--- fb8_a.c.*|--- a/drivers/video/msm/msm_fb.c|; s|^+++ fb8_b.c.*|+++ b/drivers/video/msm/msm_fb.c|' > /mnt/c/Projects/HTC-HD2-T8585/out/08-fb-clear-boot-garbage.patch
mkdir -p /tmp/t8/drivers/video/msm && cp "$F" /tmp/t8/drivers/video/msm/
cd /tmp/t8 && patch -p1 --dry-run < /mnt/c/Projects/HTC-HD2-T8585/out/08-fb-clear-boot-garbage.patch
