# Правка настройки сборки Rockbox под HTC HD2: крупный системный шрифт и
# больше памяти под буферы.
#
# Зачем шрифт. Высота строки состояния в Rockbox жёстко привязана к
# встроенному системному шрифту (STATUSBAR_HEIGHT = SYSFONT_HEIGHT), и
# настройкой её не поменять. По умолчанию для цели sdlapp берётся
# восьмипиксельный 08-Schumacher-Clean — на экране 480x800 это полоска,
# в которой ничего не разобрать. Ставим 24-Terminus-Bold: втрое выше, той
# же семьи, что и наш основной шрифт, и с кириллицей.
#
# Значки в строке состояния от шрифта НЕ зависят — это отдельные картинки
# по 16 пикселей. Их лечит своя строка состояния: wps/крупная.sbs
# (лежит рядом, out/rockbox-крупная.sbs), включается настройкой
# «sbs: крупная» в config.cfg.
#
# Зачем память. По умолчанию цель просит 8 МБ. Подняли до 64 — на всякий
# случай, запас не мешает. (Оговорка: надежда, что это починит Ogg, не
# оправдалась — там была совсем другая причина, см.
# out/rockbox-ogg-seek.patch.)
import io, sys

p = '/home/rockbox/tools/configure'
s = io.open(p, encoding='utf-8', errors='surrogateescape').read()

if 'sysfont="24-Terminus-Bold"' in s:
    print('уже пропатчено'); sys.exit(0)

L = s.split('\n')
try:
    i = [k for k, l in enumerate(L) if l.strip() == '200|sdlapp)'][0]
except IndexError:
    print('НЕ НАЙДЕН блок цели 200|sdlapp'); sys.exit(1)

try:
    j = [k for k in range(i, i + 40) if L[k].strip() == 'memory=8'][0]
except IndexError:
    print('НЕ НАЙДЕНА строка memory=8 в блоке цели'); sys.exit(1)

L[j] = '    memory=64'
L.insert(j + 1, '    sysfont="24-Terminus-Bold"')

io.open(p, 'w', encoding='utf-8', errors='surrogateescape').write('\n'.join(L))
print('готово: memory=64, sysfont=24-Terminus-Bold')
