#!/bin/sh
set -eu
W=/mnt/c/Projects/HTC-HD2-T8585
D=$HOME/lineage
mkdir -p "$D" && cd "$D"
[ -f boot.img ] || unzip -o -q "$W/lineage-15.1-20200225-UNOFFICIAL-htcleo.zip" boot.img
off=$(grep -abo $'\x1f\x8b\x08' boot.img | head -1 | cut -d: -f1)
dd if=boot.img bs=1 skip="$off" 2>/dev/null | gunzip -q > kernel.bin 2>/dev/null || true
python3 - <<'PY'
import gzip
d = open('kernel.bin','rb').read()
st = d.find(b'IKCFG_ST'); en = d.find(b'IKCFG_ED')
cfg = gzip.decompress(d[st+8:en])
open('lineage.config','wb').write(cfg)
print("конфиг извлечён:", len(cfg), "байт,", cfg.count(b'\n'), "строк")
PY
cp lineage.config "$W/out/config-lineage-htcleo"
echo "=== заголовок ==="; head -4 lineage.config
