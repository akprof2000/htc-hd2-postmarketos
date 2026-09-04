// Меню питания HTC HD2 — нативная замена powermenu на Python/Tk.
//
// Вызывается долгим нажатием красной кнопки. Кнопки делят экран поровну,
// само закрывается через 30 секунд, если про него забыли.
//
// Сборка: g++ -O2 powermenu.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o powermenu

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static const int W = 480, H = 776, WIN_Y = 24;
static const unsigned long BG = 0x101828;

struct Item { const char *label; unsigned long color; const char *cmd; };
static const Item ITEMS[] = {
    {"Выключить", 0xa4262c, "poweroff"},
    // программный reboot ПРОВЕРЕН 01.09: виснет, чипсету нужен hardboot
    {"Экран выключить", 0x33415c,
     "sh -c 'echo 0 > /sys/class/leds/lcd-backlight/brightness'"},
    {"Перезапустить графику", 0x5d5d3a, "/usr/local/bin/x-restart"},
    {"Отмена", 0x33415c, NULL},
};
static const int N = (int)(sizeof(ITEMS) / sizeof(ITEMS[0]));
static const int TOP = 74, PAD = 16, GAP = 10;

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_head, *f_btn;
static XftColor c_fg;

static void text(XftFont *fn, int x, int y, const char *s)
{
    XftDrawStringUtf8(xd, &c_fg, fn, x, y, (const FcChar8 *)s, strlen(s));
}

static int tw(XftFont *fn, const char *s)
{
    XGlyphInfo gi;
    XftTextExtentsUtf8(dpy, fn, (const FcChar8 *)s, strlen(s), &gi);
    return gi.xOff;
}

static int btn_h(void) { return (H - TOP - PAD - (N - 1) * GAP) / N; }

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);
    const char *t = "Питание";
    text(f_head, (W - tw(f_head, t)) / 2, 50, t);
    int bh = btn_h();
    for (int i = 0; i < N; i++) {
        int y = TOP + i * (bh + GAP);
        XSetForeground(dpy, gc, ITEMS[i].color);
        XFillRectangle(dpy, buf, gc, PAD, y, W - 2 * PAD, bh);
        text(f_btn, (W - tw(f_btn, ITEMS[i].label)) / 2, y + bh / 2 + 8,
             ITEMS[i].label);
    }
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

int main(void)
{
    int lock = open("/run/.powermenu.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Питание");
    XClassHint ch;
    ch.res_name = (char *)"powermenu";
    ch.res_class = (char *)"Powermenu";
    XSetClassHint(dpy, win, &ch);
    XSizeHints sh;
    sh.flags = PPosition | PSize | PMinSize | PMaxSize;
    sh.x = 0; sh.y = WIN_Y;
    sh.width = sh.min_width = sh.max_width = W;
    sh.height = sh.min_height = sh.max_height = H;
    XSetWMNormalHints(dpy, win, &sh);
    Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask);
    XMapWindow(dpy, win);
    buf = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, buf, 0, NULL);
    xd = XftDrawCreate(dpy, buf, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));
    f_head = XftFontOpenName(dpy, scr, "DejaVu Sans:size=22:bold");
    f_btn = XftFontOpenName(dpy, scr, "DejaVu Sans:size=16:bold");
    XRenderColor wc = {0xffff, 0xffff, 0xffff, 0xffff};
    XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr), &wc, &c_fg);

    int xfd = ConnectionNumber(dpy);
    time_t born = time(NULL);
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose)
                draw();
            else if (e.type == ClientMessage &&
                     (Atom)e.xclient.data.l[0] == wm_del)
                return 0;
            else if (e.type == ButtonPress) {
                int bh = btn_h();
                for (int i = 0; i < N; i++) {
                    int y = TOP + i * (bh + GAP);
                    if (e.xbutton.y < y || e.xbutton.y >= y + bh)
                        continue;
                    if (!ITEMS[i].cmd)
                        return 0;
                    // окно убираем сразу, чтобы не мешало команде
                    XUnmapWindow(dpy, win);
                    XFlush(dpy);
                    if (fork() == 0) {
                        setsid();
                        execl("/bin/sh", "sh", "-c", ITEMS[i].cmd, (char *)NULL);
                        _exit(1);
                    }
                    return 0;
                }
            }
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {1, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);
        if (time(NULL) - born > 30)     // забыли — закрываемся сами
            return 0;
    }
    return 0;
}
