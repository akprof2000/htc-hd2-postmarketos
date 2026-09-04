/* fbprobe — проверка, доходит ли рисование X до экрана.
 *
 * Зачем: телефон дважды за день оставался с горящей подсветкой и
 * ЧЁРНЫМ экраном при живом X — сервер отвечает на запросы, окна на
 * месте, но в кадровый буфер ничего не попадает. По одной яркости
 * такое не отличить от честно тёмного приложения: галерея без фото
 * даёт ровно ноль, и прежний сторож из-за этого убивал живую графику
 * вместе с домашним экраном.
 *
 * Что делает: на 0,3 секунды показывает белый квадратик 6x6 в самом
 * углу экрана и читает эти же точки из /dev/fb0. Дошло — значит тракт
 * рисования жив.
 *
 * Код возврата: 0 — рисование доходит, 1 — не доходит, 2 — X не
 * отвечает вовсе.
 *
 * Сборка: gcc -O2 fbprobe.c -lX11 -o fbprobe
 */
#include <X11/Xlib.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BOX 6

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 2;
    int scr = DefaultScreen(dpy);
    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
    /* угол внизу слева: там ничего важного, а квадратик почти незаметен */
    int px = 0, py = sh - BOX;

    XSetWindowAttributes at;
    at.override_redirect = True;          /* мимо оконного менеджера */
    at.background_pixel = WhitePixel(dpy, scr);
    Window w = XCreateWindow(dpy, RootWindow(dpy, scr), px, py, BOX, BOX, 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWOverrideRedirect | CWBackPixel, &at);
    XMapRaised(dpy, w);
    XFlush(dpy);
    usleep(300000);                       /* дать композиции дойти до панели */

    int ok = 0;
    int fd = open("/dev/fb0", O_RDONLY);
    if (fd >= 0) {
        /* 32 бита на точку, длина строки = ширина * 4 */
        long off = (long)(py + BOX / 2) * sw * 4 + (long)(px + BOX / 2) * 4;
        unsigned char p[4] = {0, 0, 0, 0};
        if (lseek(fd, off, SEEK_SET) == off && read(fd, p, 4) == 4)
            ok = (p[0] > 200 && p[1] > 200 && p[2] > 200);
        close(fd);
    }

    XDestroyWindow(dpy, w);
    XFlush(dpy);
    XCloseDisplay(dpy);
    return ok ? 0 : 1;
}
