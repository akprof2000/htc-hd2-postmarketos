#!/bin/sh
set -eu
export PATH="$HOME/.local/bin:$PATH"
cfg="$HOME/pmaports/device/archived/linux-htc-leo/config-htc-leo.armv7"
grep -n "FRAMEBUFFER_CONSOLE" "$cfg" || true
sed -i 's|^# CONFIG_FRAMEBUFFER_CONSOLE is not set$|CONFIG_FRAMEBUFFER_CONSOLE=y\nCONFIG_FRAMEBUFFER_CONSOLE_DETECT_PRIMARY=y|' "$cfg"
echo "--- после правки ---"
grep -n "FRAMEBUFFER_CONSOLE" "$cfg"
pmbootstrap checksum linux-htc-leo
echo "--- пересборка ядра ---"
pmbootstrap -y build --arch armv7 --force linux-htc-leo
echo "=== KERNEL REBUILD OK ==="
