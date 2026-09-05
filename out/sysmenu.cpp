// Системное меню HTC HD2 — нативная замена sysmenu на Python/Tk.
//
// Открывается кнопкой Windows. Активное окно запоминаем ДО того, как
// показали своё, иначе «Закрыть окно» закрыло бы само меню.
//
// Сборка: g++ -O2 sysmenu.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o sysmenu

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static const int W = 480, H = 752, WIN_Y = 48;
static const unsigned long BG = 0x101828;

// cmd == NULL — «Отмена»; cmd == "" — «закрыть запомненное окно»
struct Item { const char *label; unsigned long color; const char *cmd; };
static const Item ITEMS[] = {
    {"✕  Закрыть окно", 0xa4262c, ""},
    {"⌨  Клавиатура", 0x5133b8, "/usr/local/bin/kbd"},
    {"☀  Ярче", 0x217a6b, "/usr/local/bin/backlight +"},
    {"☾  Темнее", 0x1b4a5e, "/usr/local/bin/backlight -"},
    {"☰  Приложения", 0x0a6ebd, "/usr/local/bin/taskmgr"},
    {"◉  Питание…", 0x4a4a56, "/usr/local/bin/powermenu"},
    {"Отмена", 0x33415c, NULL},
};
static const int N = (int)(sizeof(ITEMS) / sizeof(ITEMS[0]));
static const int TOP = 66, PAD = 18, GAP = 8;

static Display *dpy;
static int scr;
static Window win, prev = 0;
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

// что было на экране до нас
static Window active_window(void)
{
    Atom a = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True), type;
    if (a == None)
        return 0;
    int fmt;
    unsigned long n, rest;
    unsigned char *data = NULL;
    Window w = 0;
    if (XGetWindowProperty(dpy, RootWindow(dpy, scr), a, 0, 1, False,
                           XA_WINDOW, &type, &fmt, &n, &rest, &data) == Success
        && data) {
        if (n > 0)
            w = *(Window *)data;
        XFree(data);
    }
    return w;
}

static int is_home(Window w)
{
    char *name = NULL;
    int home = 0;
    if (w && XFetchName(dpy, w, &name) && name) {
        home = !strcmp(name, "Домой");
        XFree(name);
    }
    return home;
}

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);
    const char *t = "Меню";
    text(f_head, (W - tw(f_head, t)) / 2, 46, t);
    int bh = btn_h();
    for (int i = 0; i < N; i++) {
        int y = TOP + i * (bh + GAP);
        XSetForeground(dpy, gc, ITEMS[i].color);
        XFillRectangle(dpy, buf, gc, PAD, y, W - 2 * PAD, bh);
        int x = ITEMS[i].cmd ? PAD + 24
                             : (W - tw(f_btn, ITEMS[i].label)) / 2;
        text(f_btn, x, y + bh / 2 + 8, ITEMS[i].label);
    }
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

int main(void)
{
    int lock = open("/run/.sysmenu.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    prev = active_window();             // запомнили ДО своего окна
    if (is_home(prev))
        prev = 0;                       // домашний экран не закрываем
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Меню");
    XClassHint ch;
    ch.res_name = (char *)"sysmenu";
    ch.res_class = (char *)"Sysmenu";
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
    f_head = XftFontOpenName(dpy, scr, "DejaVu Sans:size=20:bold");
    f_btn = XftFontOpenName(dpy, scr, "DejaVu Sans:size=15:bold");
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
                    XUnmapWindow(dpy, win);
                    XFlush(dpy);
                    if (!ITEMS[i].cmd[0]) {         // закрыть окно
                        if (prev) {
                            char cmd[96];
                            snprintf(cmd, sizeof(cmd),
                                     "wmctrl -i -c 0x%lx",
                                     (unsigned long)prev);
                            if (fork() == 0) {
                                setsid();
                                execl("/bin/sh", "sh", "-c", cmd,
                                      (char *)NULL);
                                _exit(1);
                            }
                        }
                        return 0;
                    }
                    if (fork() == 0) {
                        setsid();
                        execl("/bin/sh", "sh", "-c", ITEMS[i].cmd,
                              (char *)NULL);
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
        if (time(NULL) - born > 20)
            return 0;
    }
    return 0;
}
