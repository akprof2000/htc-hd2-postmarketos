#!/bin/bash
# Патч 10: аналоговый кодек ADIE закрывается по окончании воспроизведения и
# при следующем открытии не восстанавливает состояние — звук проходит ровно
# один раз после каждой смены маршрута. Оставляем кодек открытым: он лёгкий,
# а повторная инициализация в этой версии драйвера не работает.
set -eu
F=$HOME/leo-pmos/arch/arm/mach-msm/qdsp6_1550/q6audio.c
cp "$F" /tmp/q_a.c
python3 - <<'PY'
s=open('/tmp/q_a.c').read()
old = """static int adie_disable(void)
{
    AUDIO_INFO("%s\n", __func__);
    adie_refcount--;
    if (adie_refcount == 0)
        adie_close(adie);
    return 0;
}"""
new = """static int adie_disable(void)
{
    AUDIO_INFO("%s\n", __func__);
    adie_refcount--;
    /* НЕ закрываем кодек: после adie_close() повторный adie_open() не
     * восстанавливает аналоговый тракт, и звук проходит ровно один раз
     * за сеанс. Держим открытым — расход ничтожен. */
    return 0;
}"""
assert old in s, "anchor adie_disable"
open('/tmp/q_b.c','w').write(s.replace(old,new,1))
print("вставка ок")
PY
cd /tmp
diff -u q_a.c q_b.c | sed 's|^--- q_a.c.*|--- a/arch/arm/mach-msm/qdsp6_1550/q6audio.c|; s|^+++ q_b.c.*|+++ b/arch/arm/mach-msm/qdsp6_1550/q6audio.c|' > /mnt/c/Projects/HTC-HD2-T8585/out/10-adie-keep-open.patch
mkdir -p /tmp/t10/arch/arm/mach-msm/qdsp6_1550 && cp "$F" /tmp/t10/arch/arm/mach-msm/qdsp6_1550/
cd /tmp/t10 && patch -p1 --dry-run < /mnt/c/Projects/HTC-HD2-T8585/out/10-adie-keep-open.patch
