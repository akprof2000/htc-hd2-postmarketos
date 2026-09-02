#!/usr/bin/env python3
"""Удалённый запуск pmOS на HTC HD2 из Windows Mobile.

В WinMo с карты автозапуском (\Storage Card\2577\autorun.exe) поднимается
HaRET; в нём включается режим ожидания подключения на порту 9999 — Linux при
этом НЕ грузится. Этот скрипт дожидается, пока порт откроется, и отдаёт
команды загрузки: телефон уходит в pmOS без единого нажатия.

    python boot-pmos-remote.py [ip] [секунд_ожидания]
"""
import socket
import sys
import time

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.100.235"
WAIT = int(sys.argv[2]) if len(sys.argv) > 2 else 120
PORT = 9999

CMDS = [
    "set mtype 2524",
    "set ramaddr 0x11800000",
    "set ramsize 0x1e400000",
    r'set KERNEL "\Storage Card\PMOS\zImage"',   # пробел в пути — в кавычках
    'set cmdline "root=/dev/mmcblk0p3 rootwait rw lpj=499435 ignore_loglevel"',
    "boot",
]


def say(prefix, data):
    """Вывод без падений: консоль тут cp1251, а HaRET шлёт что придётся."""
    txt = data if isinstance(data, str) else data.decode("ascii", "replace")
    txt = txt.strip()[:200]
    enc = sys.stdout.encoding or "ascii"
    sys.stdout.write(prefix + txt.encode(enc, "replace").decode(enc) + "\n")
    sys.stdout.flush()


def main():
    # HaRET слушает одно подключение за раз, поэтому ждём его в цикле
    end = time.time() + WAIT
    sock = None
    while time.time() < end:
        try:
            sock = socket.create_connection((IP, PORT), timeout=5)
            break
        except OSError:
            time.sleep(2)
    if sock is None:
        say("", "HaRET не слушает — включите режим ожидания на телефоне")
        return 1

    sock.settimeout(4)
    try:
        say("", sock.recv(4096))
    except OSError:
        pass
    for c in CMDS:
        say("-> ", c)
        sock.send((c + "\r\n").encode())
        time.sleep(0.8)
        try:
            say("   ", sock.recv(4096))
        except OSError:
            pass
    say("", "команда загрузки отдана — телефон уходит в pmOS")
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
