#!/bin/sh
# Положить собранные бинарники на телефон:
#     sh build/поставить.sh bthfp phoned voice
# Через scp нельзя — на телефоне нет sftp-server, поэтому потоком.
# Через .new и mv — потому что заменять работающий файл на месте нельзя.
#
# Имена переменных латиницей: оболочка кириллические имена не принимает.
KEY=out/ssh/openwrt_key
HOST=root@192.168.100.235

[ $# -gt 0 ] || { echo "укажите имена программ"; exit 2; }

fail=0
for name in "$@"; do
    file="out/bin/$name"
    [ -f "$file" ] || { echo "нет $file — сначала соберите"; fail=1; continue; }
    ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -i "$KEY" "$HOST" \
        "cat > /usr/local/bin/$name.new" < "$file" &&
    ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -i "$KEY" "$HOST" \
        "chmod +x /usr/local/bin/$name.new && mv /usr/local/bin/$name.new /usr/local/bin/$name && ls -l /usr/local/bin/$name" ||
    { echo "НЕ ПОСТАВЛЕНО: $name"; fail=1; }
done
exit $fail
