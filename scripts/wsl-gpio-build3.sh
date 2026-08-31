#!/bin/bash
set -u
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap -y build --arch armv7 --force linux-htc-leo > /tmp/build.log 2>&1
echo "код: $?"
tail -30 /tmp/build.log
