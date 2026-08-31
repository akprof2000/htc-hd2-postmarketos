#!/bin/bash
tail -25 /tmp/build.log 2>/dev/null | head -12
echo "===== log.txt ====="
grep -E "ERROR|error" $HOME/.local/var/pmbootstrap/log.txt | tail -6
