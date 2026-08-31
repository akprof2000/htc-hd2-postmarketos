#!/bin/sh
set -eu
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap -y build --arch armv7 linux-htc-leo
echo "=== KERNEL BUILD OK ==="
pmbootstrap -y build --arch armv7 device-htc-leo
echo "=== DEVICE PKG OK ==="
