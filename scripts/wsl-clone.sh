#!/bin/sh
set -eu
export PATH="$HOME/.local/bin:$PATH"
d="$HOME/pmaports"
if [ -d "$d/.git" ]; then
	git -C "$d" fetch --depth=1 origin main && git -C "$d" reset --hard origin/main
else
	git clone --depth=1 -b main https://gitlab.postmarketos.org/postmarketOS/pmaports.git "$d"
fi
echo "--- htc-leo в дереве ---"
ls -d "$d"/device/*/device-htc-leo "$d"/device/*/linux-htc-leo 2>/dev/null || echo "НЕ НАЙДЕНО"
