#!/bin/sh
# Проверка одной заплатки прошивки: поднять чип с нуля, залить указанный
# файл, вернуть адрес и попробовать установить соединение с ноутбуком.
# Опрос у нас уже работает — проверяем именно соединение (paging).
FW="$1"
LAPTOP=70:D8:23:31:59:CE

echo "=== заплатка: $FW"
pkill hciattach 2>/dev/null
hciconfig hci0 down 2>/dev/null
sleep 1
echo 0 > /sys/class/rfkill/rfkill0/state 2>/dev/null; sleep 2
echo 1 > /sys/class/rfkill/rfkill0/state 2>/dev/null; sleep 2
hciattach -s 115200 /dev/ttyHS0 bcm43xx 115200 flow nosleep \
    7c:61:93:45:4b:89 >/tmp/btattach.log 2>&1 &
i=0
while [ $i -lt 25 ]; do
    grep -q 'setup complete' /tmp/btattach.log 2>/dev/null && break
    i=$((i + 1)); sleep 1
done
hciconfig hci0 up 2>/dev/null
sleep 1

/usr/local/bin/btpatch "$FW" 2>&1 | tail -2
hcitool -i hci0 cmd 0x3f 0x01 0x89 0x4b 0x45 0x93 0x61 0x7c >/dev/null 2>&1
sleep 1
hciconfig hci0 down 2>/dev/null; hciconfig hci0 up 2>/dev/null
hciconfig hci0 name 'HTC HD2' >/dev/null 2>&1
hciconfig hci0 piscan
sleep 1
echo "прошивка: $(hciconfig hci0 revision 2>/dev/null | tail -1)"

echo "--- пробую соединиться с ноутбуком:"
timeout 30 hcitool -i hci0 cc --role=m "$LAPTOP" 2>&1 | tail -2
sleep 2
CON=$(hcitool -i hci0 con 2>/dev/null | tail -n +2)
if [ -n "$CON" ]; then
    echo "СОЕДИНЕНИЕ ЕСТЬ: $CON"
else
    echo "соединения нет"
fi
