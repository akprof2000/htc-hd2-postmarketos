#!/bin/sh
# Собрать готовый Rockbox в один архив для телефона.
echo "=== установлено:"
ls -la /usr/local/bin/rockbox 2>/dev/null
du -sh /usr/local/share/rockbox 2>/dev/null
ls /usr/local/share/rockbox 2>/dev/null | head -10
echo "=== пакую:"
tar -czf /home/rockbox-app.tar.gz -C / usr/local/bin/rockbox usr/local/share/rockbox 2>/dev/null
ls -la /home/rockbox-app.tar.gz
