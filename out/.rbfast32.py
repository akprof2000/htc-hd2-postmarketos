# Rockbox на HTC HD2: промежуточные поверхности и текстура — 32 бита.
#
# Беда: экран Rockbox 24 бита, и SDL создавал lcd_surface, sim_lcd_surface
# и текстуру тоже 24-битными, а lcd_surface ещё и со смешиванием
# (SDL_BLENDMODE_BLEND). Для 24 бит у SDL нет быстрых путей копирования:
# каждое обновление экрана шло медленным попиксельным кодом (в выборке —
# деление и floorf), а окно X всё равно 32-битное — ещё одно
# преобразование. В неподвижном меню это ~19% процессора.
#
# Лечение: для LCD_DEPTH 24 все три — XRGB8888 (как окно X), смешивание
# выключено (альфа-канала у нас нет). Запуск — в окружении сборки, до make,
# после .rbfastlcd.py.
import io, sys

B = '/home/rockbox/firmware/target/hosted/sdl/'

def edit(name, pairs):
    p = B + name
    s = io.open(p, encoding='utf-8', errors='surrogateescape').read()
    if 'HD2_DEPTH32' in s:
        print('уже:', name); return
    for a, b in pairs:
        if a not in s:
            print('НЕ НАЙДЕНО в', name, ':', a); sys.exit(1)
        s = s.replace(a, b)
    io.open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s)
    print('ok:', name)

edit('lcd-bitmap.c', [
    ("""    lcd_surface = SDL_CreateRGBSurface(SDL_SWSURFACE, SIM_LCD_WIDTH, SIM_LCD_HEIGHT,
                                       LCD_DEPTH, 0, 0, 0, 0);
#if SDL_MAJOR_VERSION > 1
    SDL_SetSurfaceBlendMode(lcd_surface, SDL_BLENDMODE_BLEND);""",
     """    lcd_surface = SDL_CreateRGBSurface(SDL_SWSURFACE, SIM_LCD_WIDTH, SIM_LCD_HEIGHT,
                                       LCD_DEPTH == 24 ? 32 : LCD_DEPTH, /* HD2_DEPTH32 */
                                       0, 0, 0, 0);
#if SDL_MAJOR_VERSION > 1
    SDL_SetSurfaceBlendMode(lcd_surface, LCD_DEPTH == 24 ? SDL_BLENDMODE_NONE
                                                         : SDL_BLENDMODE_BLEND);"""),
])

edit('window-sdl.c', [
    ("depth = LCD_DEPTH < 8 ? 16 : LCD_DEPTH;",
     "depth = LCD_DEPTH < 8 ? 16 : LCD_DEPTH == 24 ? 32 : LCD_DEPTH; /* HD2_DEPTH32 */"),
])
