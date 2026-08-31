#!/bin/bash
# Патч 06: msm_fb визуал -> стандартный red@16/blue@0 (как ждёт всё ПО).
# Патч 07: mdp pack pattern BGR->RGB, чтобы физические цвета остались верны.
set -eu
FB=/home/akozlov/leo-pmos/drivers/video/msm/msm_fb.c
MD=/home/akozlov/leo-pmos/drivers/video/msm/mdp.c

# --- 06: msm_fb red@16 ---
cp "$FB" /tmp/fb_a.c
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
		/* standard RGB visual (red high) so X software packs pixels right */
		var->red.offset = 16;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;"""
assert old in s, "fb anchor"
open('/tmp/fb_b.c','w').write(s.replace(old,new,1))
PY
cd /tmp
diff -u fb_a.c fb_b.c | sed 's|^--- fb_a.c.*|--- a/drivers/video/msm/msm_fb.c|; s|^+++ fb_b.c.*|+++ b/drivers/video/msm/msm_fb.c|' > /mnt/c/Projects/HTC-HD2-T8585/out/06-fb-standard-rgb.patch

# --- 07: mdp pack BGR->RGB (+ 24 как 32) ---
cp "$MD" /tmp/md_a.c
python3 - <<'PY'
s=open('/tmp/md_a.c').read()
old="""#else
	case 24:
		format = DMA_IBUF_FORMAT_RGB888;
		pack_pattern = DMA_PACK_PATTERN_BGR;
		break;
	case 32:
		format = DMA_IBUF_FORMAT_XRGB8888;
		pack_pattern = DMA_PACK_PATTERN_BGR;
		break;
#endif"""
new="""#else
	/* fb buffer is 4 bytes/pixel; treat 24 as 32. RGB pack matches the
	 * standard red@16 visual so physical colours stay correct. */
	case 24:
	case 32:
		format = DMA_IBUF_FORMAT_XRGB8888;
		pack_pattern = DMA_PACK_PATTERN_RGB;
		break;
#endif"""
assert old in s, "mdp anchor"
open('/tmp/md_b.c','w').write(s.replace(old,new,1))
PY
cd /tmp
diff -u md_a.c md_b.c | sed 's|^--- md_a.c.*|--- a/drivers/video/msm/mdp.c|; s|^+++ md_b.c.*|+++ b/drivers/video/msm/mdp.c|' > /mnt/c/Projects/HTC-HD2-T8585/out/07-mdp-rgb-pack.patch

# dry-run обоих
mkdir -p /tmp/tt/drivers/video/msm && cp "$FB" "$MD" /tmp/tt/drivers/video/msm/
cd /tmp/tt && patch -p1 --dry-run < /mnt/c/Projects/HTC-HD2-T8585/out/06-fb-standard-rgb.patch && patch -p1 --dry-run < /mnt/c/Projects/HTC-HD2-T8585/out/07-mdp-rgb-pack.patch
