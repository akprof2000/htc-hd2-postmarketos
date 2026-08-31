#!/bin/sh
set -eu
export PATH="$HOME/.local/bin:$PATH"
# --split: отдельные boot и root образы. На карту зальём только root.
# --password: временный, сменить после первой загрузки (passwd user).
pmbootstrap -y install --split --password <TEMP_PASSWORD>
echo "=== INSTALL OK ==="
find "$HOME/.local/var/pmbootstrap" -maxdepth 4 -name 'htc-leo*.img' -exec ls -lh {} +
