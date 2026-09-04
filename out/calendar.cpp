// Календарь HTC HD2 — нативная замена calendar-app на Python/Tk.
//
// Месяц крупно, листается стрелками. Сегодняшнее число подсвечено.
//
// Сборка: g++ -O2 calendar.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o calendar

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static const int W = 480, H = 776, WIN_Y = 24;
static const unsigned long BG = 0x101828, KEY = 0x1c2a40, TODAY = 0x1f7a33;
static const int NAV_Y = 12, NAV_H = 58;
static const int WD_Y = 96;                 // строка «Пн Вт …»
static const int GRID_Y = 110, GRID_H = 640;

static const char *MON[12] = {"Январь", "Февраль", "Март", "Апрель", "Май",
                              "Июнь", "Июль", "Август", "Сентябрь",
                              "Октябрь", "Ноябрь", "Декабрь"};
static const char *WD[7] = {"Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Вс"};

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_title, *f_nav, *f_wd, *f_day;
static XftColor c_fg, c_dim, c_wend;

static int cy, cm;                          // показываемый год и месяц
static int ty, tm, td;                      // сегодня

static int leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in(int y, int m)
{
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && leap(y)) ? 29 : d[m - 1];
}

// день недели первого числа, 0 = понедельник (Целлер через mktime)
static int first_wd(int y, int m)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = 1;
    t.tm_hour = 12;
    mktime(&t);
    return (t.tm_wday + 6) % 7;
}

static void text(XftFont *fn, XftColor *c, int x, int y, const char *s)
{
    XftDrawStringUtf8(xd, c, fn, x, y, (const FcChar8 *)s, strlen(s));
}

static int tw(XftFont *fn, const char *s)
{
    XGlyphInfo gi;
    XftTextExtentsUtf8(dpy, fn, (const FcChar8 *)s, strlen(s), &gi);
    return gi.xOff;
}

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);

    XSetForeground(dpy, gc, KEY);
    XFillRectangle(dpy, buf, gc, 10, NAV_Y, 76, NAV_H);
    XFillRectangle(dpy, buf, gc, W - 86, NAV_Y, 76, NAV_H);
    text(f_nav, &c_fg, 10 + (76 - tw(f_nav, "◀")) / 2, NAV_Y + 39, "◀");
    text(f_nav, &c_fg, W - 86 + (76 - tw(f_nav, "▶")) / 2, NAV_Y + 39, "▶");
    char t[64];
    snprintf(t, sizeof(t), "%s %d", MON[cm - 1], cy);
    text(f_title, &c_fg, (W - tw(f_title, t)) / 2, NAV_Y + 38, t);

    int cw = (W - 16) / 7;
    for (int i = 0; i < 7; i++)
        text(f_wd, i > 4 ? &c_wend : &c_dim,
             8 + i * cw + (cw - tw(f_wd, WD[i])) / 2, WD_Y, WD[i]);

    int start = first_wd(cy, cm), n = days_in(cy, cm);
    int weeks = (start + n + 6) / 7;
    int rh = GRID_H / (weeks > 0 ? weeks : 1);
    for (int d = 1; d <= n; d++) {
        int cell = start + d - 1;
        int r = cell / 7, c = cell % 7;
        int x = 8 + c * cw, y = GRID_Y + r * rh;
        int cur = (d == td && cm == tm && cy == ty);
        XSetForeground(dpy, gc, cur ? TODAY : KEY);
        XFillRectangle(dpy, buf, gc, x + 2, y + 2, cw - 4, rh - 4);
        char b[8];
        snprintf(b, sizeof(b), "%d", d);
        text(f_day, &c_fg, x + (cw - tw(f_day, b)) / 2, y + rh / 2 + 8, b);
    }
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

int main(void)
{
    int lock = open("/run/.calapp.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    ty = cy = lt.tm_year + 1900;
    tm = cm = lt.tm_mon + 1;
    td = lt.tm_mday;

    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Календарь");
    XClassHint ch;
    ch.res_name = (char *)"calapp";
    ch.res_class = (char *)"Calapp";
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
    f_title = XftFontOpenName(dpy, scr, "DejaVu Sans:size=17:bold");
    f_nav = XftFontOpenName(dpy, scr, "DejaVu Sans:size=18");
    f_wd = XftFontOpenName(dpy, scr, "DejaVu Sans:size=12:bold");
    f_day = XftFontOpenName(dpy, scr, "DejaVu Sans:size=16:bold");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm_ = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XRenderColor ec = {0xefef, 0x9a9a, 0x9a9a, 0xffff};
    XftColorAllocValue(dpy, vis, cm_, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm_, &dc, &c_dim);
    XftColorAllocValue(dpy, vis, cm_, &ec, &c_wend);

    for (;;) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == Expose)
            draw();
        else if (e.type == ClientMessage &&
                 (Atom)e.xclient.data.l[0] == wm_del)
            return 0;
        else if (e.type == ButtonPress) {
            if (e.xbutton.y >= NAV_Y && e.xbutton.y < NAV_Y + NAV_H) {
                int d = 0;
                if (e.xbutton.x < 90) d = -1;
                else if (e.xbutton.x > W - 90) d = 1;
                cm += d;
                if (cm == 0) { cm = 12; cy--; }
                else if (cm == 13) { cm = 1; cy++; }
            }
            draw();
        }
    }
    return 0;
}
