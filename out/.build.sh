#!/bin/sh
cd /tmp/native || exit 1
for f in "$@"; do
    if g++ -O2 "$f.cpp" -I/usr/include/freetype2 -lX11 -lXft -lXtst \
            -lfontconfig -o "$f" 2>/tmp/native/err.txt; then
        echo "OK $f"
    else
        echo "FAIL $f"; head -20 /tmp/native/err.txt
    fi
done
