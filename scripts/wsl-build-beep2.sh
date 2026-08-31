#!/bin/bash
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap -y chroot -r -- sh -c "gcc -O2 -static -o /tmp/beep /tmp/beep.c -lm 2>&1 | head -6"
