// Калькулятор HTC HD2 — нативная замена calc на Python/Tk.
//
// Разбор выражения свой (рекурсивный спуск): скобки, четыре действия,
// приоритеты. Кнопки крупные, под палец.
//
// Сборка: g++ -O2 calc.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o calc

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include <string>

static const int W = 480, H = 776, WIN_Y = 24;
static const unsigned long BG = 0x101828, KEYC = 0x1c2a40, OPC = 0x33415c,
                           EQC = 0x1f7a33;
static const int DISP_H = 110, PAD = 8, GAP = 6;

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_disp, *f_key;
static XftColor c_fg;
static std::string expr;
static std::string shown = "0";

struct Btn { const char *label; unsigned long color; };
static const Btn KEYS[20] = {
    {"C", OPC}, {"(", OPC}, {")", OPC}, {"/", OPC},
    {"7", KEYC}, {"8", KEYC}, {"9", KEYC}, {"*", OPC},
    {"4", KEYC}, {"5", KEYC}, {"6", KEYC}, {"-", OPC},
    {"1", KEYC}, {"2", KEYC}, {"3", KEYC}, {"+", OPC},
    {"<", OPC}, {"0", KEYC}, {".", KEYC}, {"=", EQC},
};

// ── разбор выражения ─────────────────────────────────────────────────
static const char *pp;
static int perr;

static double expr_add(void);

static double expr_prim(void)
{
    while (*pp == ' ')
        pp++;
    if (*pp == '(') {
        pp++;
        double v = expr_add();
        if (*pp == ')')
            pp++;
        else
            perr = 1;
        return v;
    }
    if (*pp == '-') {
        pp++;
        return -expr_prim();
    }
    char *end = NULL;
    double v = strtod(pp, &end);
    if (end == pp) {
        perr = 1;
        return 0;
    }
    pp = end;
    return v;
}

static double expr_mul(void)
{
    double v = expr_prim();
    for (;;) {
        while (*pp == ' ')
            pp++;
        if (*pp == '*') {
            pp++;
            v *= expr_prim();
        } else if (*pp == '/') {
            pp++;
            double d = expr_prim();
            if (d == 0) {
                perr = 1;
                return 0;
            }
            v /= d;
        } else
            return v;
    }
}

static double expr_add(void)
{
    double v = expr_mul();
    for (;;) {
        while (*pp == ' ')
            pp++;
        if (*pp == '+') {
            pp++;
            v += expr_mul();
        } else if (*pp == '-') {
            pp++;
            v -= expr_mul();
        } else
            return v;
    }
}

static void evaluate(void)
{
    if (expr.empty())
        return;
    pp = expr.c_str();
    perr = 0;
    double v = expr_add();
    while (*pp == ' ')
        pp++;
    if (perr || *pp) {
        shown = "ошибка";
        expr.clear();
        return;
    }
    char b[48];
    snprintf(b, sizeof(b), "%g", v);
    expr = b;
    shown = b;
}

// ── отрисовка ────────────────────────────────────────────────────────
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

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);
    // число прижато вправо, как на обычном калькуляторе
    text(f_disp, W - 16 - tw(f_disp, shown.c_str()), 76, shown.c_str());

    int bw = (W - 2 * PAD - 3 * GAP) / 4;
    int bh = (H - DISP_H - PAD - 4 * GAP) / 5;
    for (int i = 0; i < 20; i++) {
        int c = i % 4, r = i / 4;
        int x = PAD + c * (bw + GAP), y = DISP_H + r * (bh + GAP);
        XSetForeground(dpy, gc, KEYS[i].color);
        XFillRectangle(dpy, buf, gc, x, y, bw, bh);
        text(f_key, x + (bw - tw(f_key, KEYS[i].label)) / 2,
             y + bh / 2 + 10, KEYS[i].label);
    }
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void press(const char *k)
{
    if (!strcmp(k, "C")) {
        expr.clear();
        shown = "0";
    } else if (!strcmp(k, "<")) {
        if (!expr.empty())
            expr.erase(expr.size() - 1);
        shown = expr.empty() ? "0" : expr;
    } else if (!strcmp(k, "=")) {
        evaluate();
    } else {
        expr += k;
        shown = expr;
    }
    draw();
}

int main(void)
{
    int lock = open("/tmp/.calc.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Калькулятор");
    XClassHint ch;
    ch.res_name = (char *)"calc";
    ch.res_class = (char *)"Calc";
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
    f_disp = XftFontOpenName(dpy, scr, "DejaVu Sans:size=30:bold");
    f_key = XftFontOpenName(dpy, scr, "DejaVu Sans:size=20:bold");
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr), &wc, &c_fg);

    for (;;) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == Expose)
            draw();
        else if (e.type == ClientMessage &&
                 (Atom)e.xclient.data.l[0] == wm_del)
            return 0;
        else if (e.type == ButtonPress) {
            int bw = (W - 2 * PAD - 3 * GAP) / 4;
            int bh = (H - DISP_H - PAD - 4 * GAP) / 5;
            for (int i = 0; i < 20; i++) {
                int c = i % 4, r = i / 4;
                int x = PAD + c * (bw + GAP), y = DISP_H + r * (bh + GAP);
                if (e.xbutton.x >= x && e.xbutton.x < x + bw &&
                    e.xbutton.y >= y && e.xbutton.y < y + bh) {
                    press(KEYS[i].label);
                    break;
                }
            }
        }
    }
    return 0;
}
