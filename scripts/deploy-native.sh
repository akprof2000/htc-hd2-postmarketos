#!/bin/sh
# Заливка нативных программ на телефон.
#
# Запускать из корня репозитория:  sh scripts/deploy-native.sh
# Двоичные файлы берутся из out/bin (собраны в chroot pmbootstrap:
# sudo cp out/*.cpp ~/.local/var/pmbootstrap/chroot_rootfs_htc-leo/tmp/native/
# pmbootstrap chroot -r -- /tmp/native/build.sh <имена>).
#
# scp на телефоне нет — льём через ssh + cat.

HOST=root@192.168.100.235
KEY="-i out/ssh/openwrt_key -o StrictHostKeyChecking=no"

BINS="home shade keysd statusbar radio-app rukbd phone-gui phone-sms
      calc notes sysinfo taskmgr powermenu sysmenu clock settings media
      calendar gallery game2048 btapp camera"

for f in $BINS; do
    [ -f "out/bin/$f" ] || continue
    if ssh $KEY $HOST "cat > /usr/local/bin/$f.n && chmod +x /usr/local/bin/$f.n \
        && mv /usr/local/bin/$f.n /usr/local/bin/$f" < "out/bin/$f"; then
        echo "залито: $f"
    else
        echo "НЕ ЗАЛИЛОСЬ: $f" >&2
    fi
done

# camshot — конвейер снимка, остался на Python (numpy/PIL)
if [ -f out/camshot ]; then
    ssh $KEY $HOST "cat > /usr/local/bin/camshot && chmod +x /usr/local/bin/camshot" \
        < out/camshot && echo "залито: camshot"
fi

# службы поднимаем по одной и РАЗНЫМИ вызовами ssh: pkill по имени в той
# же команде, где стоит путь к программе, убивает собственную сессию
for s in '[h]ome' '[k]eysd' '[s]tatusbar' '[s]hade'; do
    # первая буква в скобках, иначе pkill находит саму себя
    ssh $KEY $HOST "pkill -f '$s' 2>/dev/null; true" >/dev/null 2>&1
done
echo "готово — службы перезапустит uisup"
