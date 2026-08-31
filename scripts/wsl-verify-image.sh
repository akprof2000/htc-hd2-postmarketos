set -eu
OUT="$HOME/htc-leo-sdcard.img"
lo=$(sudo losetup --show -f -P "$OUT")
trap 'sudo umount /tmp/vm3 /tmp/vm2 2>/dev/null || true; sudo losetup -d "$lo"' EXIT
mkdir -p /tmp/vm2 /tmp/vm3
sudo mount "${lo}p3" /tmp/vm3
echo "=== pmOS_root: os-release ==="
sudo cat /tmp/vm3/etc/os-release | head -4
echo "=== корень ==="
sudo ls /tmp/vm3
echo "=== fstab ==="
sudo cat /tmp/vm3/etc/fstab
echo "=== свободно в корне ==="
df -h /tmp/vm3 | tail -1
sudo mount "${lo}p2" /tmp/vm2
echo "=== pmOS_boot ==="
sudo ls -lh /tmp/vm2
