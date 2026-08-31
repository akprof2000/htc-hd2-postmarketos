#!/bin/bash
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap --offline -y build --arch armv7 --force linux-htc-leo > $HOME/b.log 2>&1
echo "код: $?"
grep -B4 -E 'Error [12]' $HOME/b.log | head -18
grep -E 'error:' $HOME/b.log | head -8
