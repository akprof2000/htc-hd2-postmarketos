#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap -y chroot -r -- sh -c "cd /kexec-tools && make CFLAGS='-O2 -fcommon -fno-pie' LDFLAGS='-static -no-pie' 2>&1 | tail -12"
