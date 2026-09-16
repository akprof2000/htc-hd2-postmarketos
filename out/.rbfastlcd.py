# Быстрый вывод кадра в Rockbox «как приложение» для экрана 24 бита.
#
# Беда: в firmware/target/hosted/sdl/lcd-sdl.c быстрый путь (весь кадр
# одной операцией SDL_BlitSurface) есть только для 16-битного RGB565. У
# цели sdlapp экран LCD_DEPTH 24, LCD_PIXELFORMAT RGB888, поэтому каждое
# обновление экрана шло «очень медленным попиксельным рисованием» —
# отдельным SDL_FillRect на каждую точку. На экране 480x800 это до 384 000
# вызовов за одно обновление: Rockbox в простое грузил процессор на
# 30% и тормозил (проверено выборкой адресов: SDL_FillRect/FillRects и
# sdl_update_rect).
#
# Лечение: для 24 бит тоже весь кадр одной операцией. Пиксель буфера —
# struct { unsigned char b, g, r; } (lcd.h), в памяти байты B, G, R — это
# SDL_PIXELFORMAT_BGR24; строки подряд (HORIZONTAL_STRIDE).
#
# Запуск — внутри окружения сборки, до make.
import io, sys

p = '/home/rockbox/firmware/target/hosted/sdl/lcd-sdl.c'
s = io.open(p, encoding='utf-8', errors='surrogateescape').read()

if 'SDL_PIXELFORMAT_BGR24' in s:
    print('уже пропатчено'); sys.exit(0)

anchor = """    dest = src;
    SDL_BlitSurface(lcd, &src, surface, &dest);
    SDL_FreeSurface(lcd);
#else
    int x, y;
    int xmax, ymax;
    /* Very slow pixel-by-pixel drawing */"""

if anchor not in s:
    print('НЕ НАЙДЕНО место быстрого пути в lcd-sdl.c'); sys.exit(1)

new = """    dest = src;
    SDL_BlitSurface(lcd, &src, surface, &dest);
    SDL_FreeSurface(lcd);
#elif LCD_DEPTH == 24 && (LCD_PIXELFORMAT == RGB888) && \\
    (LCD_STRIDEFORMAT == HORIZONTAL_STRIDE) && \\
    !defined(HAVE_LCD_SPLIT) && !defined(HAVE_REMOTE_LCD) && \\
    SDL_MAJOR_VERSION > 1
    /* HTC HD2: экран 24 бита. Весь кадр одной операцией, а не по
     * SDL_FillRect на каждую точку. Пиксель буфера — {b, g, r}, в памяти
     * байты B, G, R: это SDL_PIXELFORMAT_BGR24. */
    SDL_Rect src;
    (void)max_x;
    (void)max_y;
    (void)getpixel;
    SDL_Surface *lcd = SDL_CreateRGBSurfaceWithFormatFrom(FBADDR(0, 0),
                                                          LCD_FBWIDTH,
                                                          LCD_FBHEIGHT, 24,
                                                          LCD_FBWIDTH * 3,
                                                          SDL_PIXELFORMAT_BGR24);
    if (lcd) {
        src.x = x_start;
        src.y = y_start;
        src.w = width;
        src.h = height;
        dest = src;
        SDL_BlitSurface(lcd, &src, surface, &dest);
        SDL_FreeSurface(lcd);
    }
#else
    int x, y;
    int xmax, ymax;
    /* Very slow pixel-by-pixel drawing */"""

s = s.replace(anchor, new, 1)
io.open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s)
print('готово: быстрый вывод кадра для 24 бит')
