#!/bin/bash
# Патч 11: у динамика гарнитуры Bluetooth (ADSP_AUDIO_DEVICE_ID_BT_SCO_SPKR)
# в таблице устройств q6audio_devices.h стоит частота 48000 — запись явно
# скопирована с соседней A2DP. Голосовой канал Bluetooth работает на 8000:
# с 48000 звуковой процессор загружает настройки не для той частоты и
# выдаёт на линию PCM звук не в том темпе — в гарнитуру не доходит ничего.
# У микрофона той же гарнитуры (BT_SCO_MIC) частота правильная, 8000, и он
# работает: собеседник слышит, а в гарнитуре тишина.
#
# Скрипт: делает патч из ~/leo-pmos, добавляет его в APKBUILD пакета ядра,
# собирает пакет и извлекает ядро в out/zImage-btsco.
set -eu
export PATH="$HOME/.local/bin:$PATH"
P=/mnt/c/Projects/HTC-HD2-T8585
F=$HOME/leo-pmos/arch/arm/mach-msm/qdsp6_1550/q6audio_devices.h

cp "$F" /tmp/d_a.h
python3 - <<'PY'
import re
s = open('/tmp/d_a.h').read()
# только в пределах записи BT_SCO_SPKR — [^}] не даёт уйти в соседнюю запись
pat = r'(\.id\s*=\s*ADSP_AUDIO_DEVICE_ID_BT_SCO_SPKR,[^}]*?\.rate\s*=\s*)48000'
s2, n = re.subn(pat, r'\g<1>8000', s, count=1)
assert n == 1, 'не найдена запись BT_SCO_SPKR с частотой 48000'
open('/tmp/d_b.h', 'w').write(s2)
print('правка частоты ок')
PY

cd /tmp
diff -u d_a.h d_b.h \
  | sed 's|^--- d_a.h.*|--- a/arch/arm/mach-msm/qdsp6_1550/q6audio_devices.h|; s|^+++ d_b.h.*|+++ b/arch/arm/mach-msm/qdsp6_1550/q6audio_devices.h|' \
  > "$P/out/11-bt-sco-spkr-rate.patch"
echo "=== патч:"
cat "$P/out/11-bt-sco-spkr-rate.patch"

# проверка, что патч ложится на чистый файл
rm -rf /tmp/t11 && mkdir -p /tmp/t11/arch/arm/mach-msm/qdsp6_1550
cp "$F" /tmp/t11/arch/arm/mach-msm/qdsp6_1550/
(cd /tmp/t11 && patch -p1 --dry-run < "$P/out/11-bt-sco-spkr-rate.patch")

A="$HOME/pmaports/device/archived/linux-htc-leo"
cp "$P/out/11-bt-sco-spkr-rate.patch" "$A/"
if ! grep -q '11-bt-sco-spkr-rate' "$A/APKBUILD"; then
  awk '/^\t10-adie-keep-open\.patch$/ { print; print "\t11-bt-sco-spkr-rate.patch"; next } { print }' \
    "$A/APKBUILD" > "$A/APKBUILD.new" && mv "$A/APKBUILD.new" "$A/APKBUILD"
fi
grep -q '11-bt-sco-spkr-rate' "$A/APKBUILD" || { echo "НЕ ВСТАВИЛСЯ в APKBUILD"; exit 1; }
echo "=== патчи в APKBUILD:"
grep -E '^\s*(09|10|11)-' "$A/APKBUILD"

set +e
if ! pmbootstrap -y checksum linux-htc-leo > "$HOME/c11.log" 2>&1; then
  echo "ПЕРЕСЧЁТ КОНТРОЛЬНЫХ СУММ УПАЛ:"
  tail -20 "$HOME/c11.log"
  exit 1
fi
echo "контрольные суммы пересчитаны"
# Без --offline: pmbootstrap в строгом режиме перед сборкой сносит окружения
# сборки и пересоздаёт служебное x86_64, а его пакетов в кэше нет — офлайн
# сборка падала на «unable to select packages».
pmbootstrap -y build --arch armv7 --force linux-htc-leo > "$HOME/b11.log" 2>&1
rc=$?
set -e
echo "код сборки: $rc"
grep -E 'Done|error:|malformed|FAILED|Hunk|patch failed' "$HOME/b11.log" | tail -5
[ $rc -eq 0 ] || exit 1

K=$(ls -t "$HOME"/.local/var/pmbootstrap/packages/edge/armv7/linux-htc-leo-*.apk | head -1)
rm -rf /tmp/kk && mkdir /tmp/kk && tar xzf "$K" -C /tmp/kk 2>/dev/null || true
cp /tmp/kk/boot/vmlinuz* "$P/out/zImage-btsco"
echo "=== ядра:"
md5sum "$P/out/zImage-btsco" "$P/out/zImage-final"
echo "ГОТОВО"
