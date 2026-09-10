#!/bin/sh
# Собрать программу из out/ в окружении pmbootstrap и вынести бинарник.
# Запускается со стороны Windows:  wsl -d Ubuntu -- bash build/собрать.sh bta2dp
#
# Тип определяем по расширению: .c собираем gcc, .cpp — g++ с иксовыми
# библиотеками. Готовый бинарник кладём рядом с исходником, в out/bin/.
set -e
export PATH="$HOME/.local/bin:$PATH"

ИМЯ="$1"
[ -n "$ИМЯ" ] || { echo "укажите имя программы, например: bta2dp"; exit 2; }

ПРОЕКТ=/mnt/c/Projects/HTC-HD2-T8585
ОКР=$(ls -d "$HOME"/.local/var/pmbootstrap/chroot_rootfs_htc-leo 2>/dev/null)
[ -n "$ОКР" ] || { echo "окружение pmbootstrap не найдено"; exit 1; }

if [ -f "$ПРОЕКТ/out/$ИМЯ.c" ]; then
    ИСХ="$ИМЯ.c"
    # -lsbc нужен тем, кто кодирует звук; лишним он не бывает
    КОМАНДА="gcc -O2 /tmp/$ИСХ -o /tmp/$ИМЯ -lsbc"
elif [ -f "$ПРОЕКТ/out/$ИМЯ.cpp" ]; then
    ИСХ="$ИМЯ.cpp"
    КОМАНДА="g++ -O2 /tmp/$ИСХ -I/usr/include/freetype2 -lX11 -lXft -lXtst -lfontconfig -o /tmp/$ИМЯ"
else
    echo "не нашёл ни out/$ИМЯ.c, ни out/$ИМЯ.cpp"; exit 1
fi

sudo cp "$ПРОЕКТ/out/$ИСХ" "$ОКР/tmp/$ИСХ"
pmbootstrap -y chroot -r -- sh -c "$КОМАНДА" 2>&1 | grep -E 'error|ошибк' || true

if ! sudo test -f "$ОКР/tmp/$ИМЯ"; then
    echo "НЕ СОБРАЛОСЬ: $ИМЯ"; exit 1
fi

mkdir -p "$ПРОЕКТ/out/bin"
sudo cp "$ОКР/tmp/$ИМЯ" "$ПРОЕКТ/out/bin/$ИМЯ"
sudo chmod 666 "$ПРОЕКТ/out/bin/$ИМЯ"
echo "собрано: out/bin/$ИМЯ"
