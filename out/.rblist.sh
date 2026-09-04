#!/bin/sh
# что реально видно внутри сборочного окружения
echo "содержимое /home/rockbox:"
ls /home/rockbox
echo "размер:"
du -sh /home/rockbox 2>/dev/null
