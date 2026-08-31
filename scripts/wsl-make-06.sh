#!/bin/bash
# Патч 06: панель HD2 физически BGR - меняем red/blue offset в 32/24bpp ветке
# msmfb_set_par (драйвер жёстко навязывает RGB, перекрывая любой ioctl).
set -eu
F=/home/akozlov/leo-pmos/drivers/video/msm/msm_fb.c
cp "$F" /tmp/fb_a.c
python3 - <<'PY'
s=open('/tmp/fb_a.c').read()
old="""	if (var->bits_per_pixel == 32 || var->bits_per_pixel == 24) {
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 16;
		var->blue.length = 8;"""
new="""	if (var->bits_per_pixel == 32 || var->bits_per_pixel == 24) {
		/* HD2 panel is physically BGR: swap red/blue vs upstream RGB */
		var->red.offset = 16;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;"""
assert old in s, "якорь set_par не найден"
open('/tmp/fb_b.c','w').write(s.replace(old,new,1))
print("set_par ок")
PY
cd /tmp
diff -u fb_a.c fb_b.c | sed 's|^--- fb_a.c.*|--- a/drivers/video/msm/msm_fb.c|; s|^+++ fb_b.c.*|+++ b/drivers/video/msm/msm_fb.c|' > /mnt/c/Projects/HTC-HD2-T8585/out/06-panel-bgr-colors.patch
head -3 /mnt/c/Projects/HTC-HD2-T8585/out/06-panel-bgr-colors.patch
# dry-run
mkdir -p /tmp/t6/drivers/video/msm && cp "$F" /tmp/t6/drivers/video/msm/
cd /tmp/t6 && patch -p1 --dry-run < /mnt/c/Projects/HTC-HD2-T8585/out/06-panel-bgr-colors.patch
