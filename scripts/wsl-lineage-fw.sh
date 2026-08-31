#!/bin/sh
set -eu
W=/mnt/c/Projects/HTC-HD2-T8585
D=$HOME/lineage
cd "$D"
command -v brotli >/dev/null || { sudo apt-get install -y -qq brotli >/dev/null 2>&1; }
[ -f sdat2img.py ] || curl -sL -o sdat2img.py https://raw.githubusercontent.com/xpirt/sdat2img/master/sdat2img.py
[ -f system.new.dat.br ] || unzip -o -q "$W/lineage-15.1-20200225-UNOFFICIAL-htcleo.zip" system.new.dat.br system.transfer.list
[ -f system.new.dat ] || brotli -d -f system.new.dat.br -o system.new.dat
ls -lh system.new.dat
[ -f system.img ] || python3 sdat2img.py system.transfer.list system.new.dat system.img 2>&1 | tail -3
ls -lh system.img
m=$(mktemp -d)
sudo mount -o loop,ro system.img "$m" 2>&1
echo "=== прошивки в образе ==="
sudo find "$m" -iname "*bcmdhd*" -o -iname "*bcm4329*" -o -iname "*.hcd" 2>/dev/null | head -10
mkdir -p "$W/out/fw-lineage"
sudo find "$m" -iname "fw_bcmdhd*" -exec cp {} "$W/out/fw-lineage/" \; 2>/dev/null
sudo find "$m" -iname "*bcm4329*" -exec cp {} "$W/out/fw-lineage/" \; 2>/dev/null
sudo chown -R 1000:1000 "$W/out/fw-lineage" 2>/dev/null || true
sudo umount "$m"; rmdir "$m"
echo "=== скопировано ==="
ls -l "$W/out/fw-lineage/"
