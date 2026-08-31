#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap -y build --arch armv7 --force linux-htc-leo 2>&1 | tail -6
echo "=== лог ошибок ==="
grep -E 'FAILED|error:|Hunk|Reversed|rejects' $HOME/.local/var/pmbootstrap/log.txt | tail -8
