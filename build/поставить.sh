#!/bin/sh
# Положить собранный бинарник на телефон.
#   build/поставить.sh bta2dp
# Через scp нельзя — на телефоне нет sftp-server, поэтому потоком.
# Через .new и mv — потому что заменять работающий файл на месте нельзя.
set -e
ИМЯ="$1"
[ -n "$ИМЯ" ] || { echo "укажите имя программы"; exit 2; }

КЛЮЧ=out/ssh/openwrt_key
АДРЕС=root@192.168.100.235
ФАЙЛ="out/bin/$ИМЯ"
[ -f "$ФАЙЛ" ] || { echo "нет $ФАЙЛ — сначала соберите"; exit 1; }

ssh -o StrictHostKeyChecking=no -i "$КЛЮЧ" "$АДРЕС" \
    "cat > /usr/local/bin/$ИМЯ.new" < "$ФАЙЛ"
ssh -o StrictHostKeyChecking=no -i "$КЛЮЧ" "$АДРЕС" \
    "chmod +x /usr/local/bin/$ИМЯ.new && mv /usr/local/bin/$ИМЯ.new /usr/local/bin/$ИМЯ && ls -l /usr/local/bin/$ИМЯ"
