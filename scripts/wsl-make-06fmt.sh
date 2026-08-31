#!/bin/bash
# Патч 06: при глубине 24 MDP брал RGB888 (3 байта/пиксель), а буфер всегда
# 4 байта -> рассинхрон -> серое становится оранжевым в X. Делаем case 24
# идентичным case 32 (XRGB8888, 4 байта).
set -eu
F=/home/akozlov/leo-pmos/drivers/video/msm/mdp.c
cp "$F" /tmp/mdp_a.c
python3 - <<'PY'
s=open('/tmp/mdp_a.c').read()
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
	/* fb хранится 4 байта/пиксель всегда: 24 трактуем как 32, иначе MDP
	 * читает буфер со сдвигом и цвета X уходят в оранжевый */
	case 24:
	case 32:
		format = DMA_IBUF_FORMAT_XRGB8888;
		pack_pattern = DMA_PACK_PATTERN_BGR;
		break;
#endif"""
assert old in s, "якорь mdp не найден"
open('/tmp/mdp_b.c','w').write(s.replace(old,new,1))
print("mdp ок")
PY
cd /tmp
diff -u mdp_a.c mdp_b.c | sed 's|^--- mdp_a.c.*|--- a/drivers/video/msm/mdp.c|; s|^+++ mdp_b.c.*|+++ b/drivers/video/msm/mdp.c|' > /mnt/c/Projects/HTC-HD2-T8585/out/06-mdp-24bpp-as-32.patch
mkdir -p /tmp/t6/drivers/video/msm && cp "$F" /tmp/t6/drivers/video/msm/
cd /tmp/t6 && patch -p1 --dry-run < /mnt/c/Projects/HTC-HD2-T8585/out/06-mdp-24bpp-as-32.patch
