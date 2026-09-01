#!/usr/bin/env python3
"""Удалённый запуск pmOS на HTC HD2 из Windows Mobile.

В WinMo из автозагрузки поднимается HaRET в режиме LISTEN (порт 9999) —
Linux он при этом НЕ грузит. Этот скрипт подключается к нему по сети и
отдаёт команды загрузки: телефон уходит в pmOS без единого нажатия.

    python boot-pmos-remote.py [ip]
"""
import socket
import sys
import time

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.100.235"
PORT = 9999

CMDS = [
    "set mtype 2524",
    "set ramaddr 0x11800000",
    "set ramsize 0x1e400000",
    r"set KERNEL \Storage Card\PMOS\zImage",
    'set cmdline "root=/dev/mmcblk0p3 rootwait rw lpj=499435 ignore_loglevel"',
    "boot",
]


def main():
    print("подключаюсь к HaRET %s:%d …" % (IP, PORT))
    s = socket.create_connection((IP, PORT), timeout=15)
    s.settimeout(5)
    time.sleep(1)
    try:
        print(s.recv(4096).decode("ascii", "replace").strip()[:200])
    except socket.timeout:
        pass
    for c in CMDS:
        print("-> %s" % c)
        s.send((c + "\r\n").encode())
        time.sleep(0.7)
        try:
            out = s.recv(4096).decode("ascii", "replace").strip()
            if out:
                print("   %s" % out[:160])
        except socket.timeout:
            pass
    print("команда загрузки отдана — телефон уходит в pmOS")
    s.close()


if __name__ == "__main__":
    main()
