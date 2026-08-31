#!/usr/bin/env python3
"""Готовит патч 10: не закрывать аналоговый кодек ADIE.

После adie_close() повторный adie_open() не восстанавливает аналоговый тракт,
поэтому звук проходит ровно один раз за сеанс — и в динамик, и в наушники.
Держим кодек открытым: расход ничтожен, а звук становится повторяемым.
"""
import os
import subprocess

SRC = os.path.expanduser(
    "~/leo-pmos/arch/arm/mach-msm/qdsp6_1550/q6audio.c")
OUT = "/mnt/c/Projects/HTC-HD2-T8585/out/10-adie-keep-open.patch"

lines = open(SRC).read().split("\n")
out, i, done = [], 0, False

while i < len(lines):
    line = lines[i]
    if not done and line.startswith("static int adie_disable(void)"):
        # копируем тело до строки с adie_close
        while i < len(lines) and "adie_close" not in lines[i]:
            out.append(lines[i])
            i += 1
        # убираем условие, которое вело к закрытию
        while out and "adie_refcount == 0" in out[-1]:
            out.pop()
        out.append("    /* НЕ закрываем кодек: после adie_close() повторный")
        out.append("     * adie_open() не восстанавливает аналоговый тракт, и")
        out.append("     * звук проходит ровно один раз за сеанс. Держим")
        out.append("     * открытым — расход ничтожен. */")
        i += 1          # пропускаем саму строку adie_close
        done = True
        continue
    out.append(line)
    i += 1

assert done, "adie_disable не найден"

open("/tmp/q_a.c", "w").write("\n".join(lines))
open("/tmp/q_b.c", "w").write("\n".join(out))

diff = subprocess.run(["diff", "-u", "/tmp/q_a.c", "/tmp/q_b.c"],
                      capture_output=True, text=True).stdout
diff = diff.replace("--- /tmp/q_a.c",
                    "--- a/arch/arm/mach-msm/qdsp6_1550/q6audio.c", 1)
diff = diff.replace("+++ /tmp/q_b.c",
                    "+++ b/arch/arm/mach-msm/qdsp6_1550/q6audio.c", 1)
# убрать метки времени из заголовков
diff = "\n".join(l.split("\t")[0] if l.startswith(("---", "+++")) else l
                 for l in diff.split("\n"))
open(OUT, "w").write(diff)
print("патч записан, строк:", len(diff.split("\n")))
