#!/bin/sh
for f in /tmp/native/.probe /tmp/.probe2 /home/rbtest/.probe3; do
    [ -f "$f" ] && echo "$f — ВИДНО" || echo "$f — не видно"
done
echo "--- что вообще в /tmp:"; ls /tmp | head -6
echo "--- что в /home:"; ls /home | head -6
