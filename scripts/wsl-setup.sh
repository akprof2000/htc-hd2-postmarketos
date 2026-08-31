#!/bin/sh
# Подготовка WSL Ubuntu под сборку postmarketOS для htc-leo.
set -eu
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -qq
sudo apt-get install -y -qq parted dosfstools e2fsprogs qemu-user-static
pipx install pmbootstrap || pipx upgrade pmbootstrap
pipx ensurepath >/dev/null 2>&1 || true
echo "--- версии ---"
parted --version | head -1
"$HOME/.local/bin/pmbootstrap" --version
