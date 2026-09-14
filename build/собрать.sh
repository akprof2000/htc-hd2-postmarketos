#!/bin/sh
# Собрать программы из out/ в окружении pmbootstrap и вынести бинарники.
# Запускается со стороны Windows, из корня проекта:
#     wsl -d Ubuntu -- sh build/собрать.sh bta2dp phoned voice
#
# Тип определяем по расширению: .c собираем gcc, .cpp — g++ с иксовыми
# библиотеками. Готовые бинарники — в out/bin/.
#
# Имена переменных латиницей: оболочка кириллические имена не принимает.
export PATH="$HOME/.local/bin:$PATH"

[ $# -gt 0 ] || { echo "укажите имена программ, например: bta2dp phoned"; exit 2; }

PROJ=/mnt/c/Projects/HTC-HD2-T8585
ENV="$HOME/.local/var/pmbootstrap/chroot_rootfs_htc-leo"
[ -d "$ENV" ] || { echo "окружение pmbootstrap не найдено: $ENV"; exit 1; }
mkdir -p "$PROJ/out/bin"

fail=0
for name in "$@"; do
    if [ -f "$PROJ/out/$name.c" ]; then
        src="$name.c"
        # -lsbc нужен тем, кто кодирует звук; остальным не мешает
        cmd="gcc -O2 /tmp/$src -o /tmp/$name -lsbc"
    elif [ -f "$PROJ/out/$name.cpp" ]; then
        src="$name.cpp"
        cmd="g++ -O2 /tmp/$src -I/usr/include/freetype2 -lX11 -lXft -lXtst -lfontconfig -o /tmp/$name"
    else
        echo "НЕТ ИСХОДНИКА: out/$name.c или out/$name.cpp"; fail=1; continue
    fi
    sudo rm -f "$ENV/tmp/$name"
    sudo cp "$PROJ/out/$src" "$ENV/tmp/$src"
    pmbootstrap -q -y chroot -r -- sh -c "$cmd" 2>&1 | grep -E 'error|warning: implicit' || true
    if sudo test -f "$ENV/tmp/$name"; then
        sudo cp "$ENV/tmp/$name" "$PROJ/out/bin/$name"
        sudo chmod 666 "$PROJ/out/bin/$name"
        echo "собрано: out/bin/$name"
    else
        echo "НЕ СОБРАЛОСЬ: $name"; fail=1
    fi
done
exit $fail
