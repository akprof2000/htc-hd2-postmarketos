// Экранная клавиатура HTC HD2 (ЙЦУКЕН / QWERTY / цифры) — нативная.
//
// Прежняя версия на Python запускала xdotool на КАЖДУЮ букву: процесс на
// символ, отсюда задержки. Здесь символ отправляется напрямую через
// XTEST: свободному коду клавиши временно назначается нужный keysym,
// дальше нажатие-отпускание. Кириллица работает так же, как латиница.
//
// Сборка: g++ -O2 rukbd.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lXtst -lfontconfig -o rukbd

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/XTest.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

static const int W = 480, H = 288, WIN_Y = 800 - H;
static const unsigned long BG = 0x0d1420, KEYBG = 0x1e2c42, ACT = 0x33517c;

// Раскладки: строки в UTF-8, разбираются посимвольно
static const char *RU[3] = {"йцукенгшщзхъ", "фывапролджэ", "ячсмитьбю"};
static const char *EN[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char *NUM[3] = {"1234567890", "-/:;()&@\"'", ".,?!#%+*=_"};

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_key;
static XftColor c_fg;
static int layer = 0;                  // 0 ru, 1 en, 2 num
static int shift = 0;

// ── раскладка клавиш ─────────────────────────────────────────────────
struct Key {
    int x, y, w, h;
    std::string label;                 // что рисуем
    std::string ch;                    // что печатаем (UTF-8), пусто — команда
    int cmd;                           // 1 shift, 2 backspace, 3 слой,
};                                     // 4 пробел, 5 ввод, 6 закрыть

static std::vector<Key> keys;

// разбор UTF-8 на символы
static std::vector<std::string> utf8_chars(const char *s)
{
    std::vector<std::string> out;
    while (*s) {
        int n = 1;
        unsigned char c = *s;
        if ((c & 0xe0) == 0xc0) n = 2;
        else if ((c & 0xf0) == 0xe0) n = 3;
        else if ((c & 0xf8) == 0xf0) n = 4;
        out.push_back(std::string(s, n));
        s += n;
    }
    return out;
}

// заглавная буква для кириллицы и латиницы (двухбайтовый UTF-8)
static std::string upper(const std::string &c)
{
    if (c.size() == 1 && c[0] >= 'a' && c[0] <= 'z')
        return std::string(1, c[0] - 32);
    if (c.size() == 2) {
        unsigned cp = ((c[0] & 0x1f) << 6) | (c[1] & 0x3f);
        if (cp >= 0x430 && cp <= 0x44f)        // а-я -> А-Я
            cp -= 0x20;
        else if (cp == 0x451)                  // ё -> Ё
            cp = 0x401;
        else
            return c;
        std::string r;
        r += (char)(0xc0 | (cp >> 6));
        r += (char)(0x80 | (cp & 0x3f));
        return r;
    }
    return c;
}

static void build_keys(void)
{
    keys.clear();
    const char **rows = layer == 0 ? RU : (layer == 1 ? EN : NUM);
    int rh = H / 4;
    for (int r = 0; r < 3; r++) {
        std::vector<std::string> ch = utf8_chars(rows[r]);
        int extra = (r == 2 && layer != 2) ? 4 : 0;   // Shift(2) + ⌫(2)
        int units = (int)ch.size() + extra;
        int uw = W / units;
        int x = 0, y = r * rh;
        if (r == 2 && layer != 2) {
            Key k = {0, y, uw * 2, rh, "⇧", "", 1};
            keys.push_back(k);
            x = uw * 2;
        }
        for (size_t i = 0; i < ch.size(); i++) {
            std::string c = shift ? upper(ch[i]) : ch[i];
            Key k = {x, y, uw, rh, c, c, 0};
            keys.push_back(k);
            x += uw;
        }
        if (r == 2 && layer != 2) {
            Key k = {x, y, W - x, rh, "<-", "", 2};
            keys.push_back(k);
        }
    }
    // нижний ряд: смена слоя, пробел, ввод, закрыть
    int y = 3 * rh, units = 16, uw = W / units;
    const char *nxt = layer == 0 ? "ENG" : (layer == 1 ? "123" : "РУС");
    Key a = {0, y, uw * 3, H - y, nxt, "", 3};
    Key b = {uw * 3, y, uw * 8, H - y, "___", " ", 4};
    Key c = {uw * 11, y, uw * 3, H - y, "OK", "", 5};
    Key d = {uw * 14, y, W - uw * 14, H - y, "X", "", 6};
    keys.push_back(a);
    keys.push_back(b);
    keys.push_back(c);
    keys.push_back(d);
}

// ── ввод символов через XTEST ────────────────────────────────────────
static int spare_code = 0;             // свободный код клавиши для подмены

static void find_spare_code(void)
{
    int lo, hi;
    XDisplayKeycodes(dpy, &lo, &hi);
    int per = 0;
    KeySym *map = XGetKeyboardMapping(dpy, lo, hi - lo + 1, &per);
    if (!map)
        return;
    for (int i = hi - lo; i >= 0 && !spare_code; i--) {
        int empty = 1;
        for (int j = 0; j < per; j++)
            if (map[i * per + j])
                empty = 0;
        if (empty)
            spare_code = lo + i;
    }
    XFree(map);
    if (!spare_code)
        spare_code = hi;               // хоть какой-то
}

static unsigned utf8_to_cp(const std::string &s)
{
    unsigned char c = s[0];
    if (c < 0x80) return c;
    if ((c & 0xe0) == 0xc0) return ((c & 0x1f) << 6) | (s[1] & 0x3f);
    if ((c & 0xf0) == 0xe0)
        return ((c & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
    return 0;
}

static void send_keysym(KeySym ks)
{
    if (!spare_code)
        return;
    KeySym two[2] = {ks, ks};
    XChangeKeyboardMapping(dpy, spare_code, 2, two, 1);
    XSync(dpy, False);
    XTestFakeKeyEvent(dpy, spare_code, True, 0);
    XTestFakeKeyEvent(dpy, spare_code, False, 0);
    XSync(dpy, False);
}

static void send_char(const std::string &c)
{
    unsigned cp = utf8_to_cp(c);
    if (!cp)
        return;
    // для символов вне latin-1 keysym = код символа + 0x01000000
    KeySym ks = (cp < 0x100) ? (KeySym)cp : (KeySym)(cp | 0x01000000);
    send_keysym(ks);
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
    for (size_t i = 0; i < keys.size(); i++) {
        const Key &k = keys[i];
        XSetForeground(dpy, gc, (k.cmd == 1 && shift) ? ACT : KEYBG);
        XFillRectangle(dpy, buf, gc, k.x + 1, k.y + 1, k.w - 2, k.h - 2);
        text(f_key, k.x + (k.w - tw(f_key, k.label.c_str())) / 2,
             k.y + k.h / 2 + 8, k.label.c_str());
    }
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

int main(void)
{
    int lock = open("/run/.rukbd.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "нет доступа к X\n");
        return 1;
    }
    int ev, err, major, minor;
    if (!XTestQueryExtension(dpy, &ev, &err, &major, &minor)) {
        fprintf(stderr, "нет расширения XTEST\n");
        return 1;
    }
    scr = DefaultScreen(dpy);
    find_spare_code();

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "rukbd");
    XClassHint ch;
    ch.res_name = (char *)"rukbd";
    ch.res_class = (char *)"Rukbd";
    XSetClassHint(dpy, win, &ch);
    // фокус нам не нужен: печатаем в чужое окно, своё не должно его отбирать
    XWMHints wmh;
    wmh.flags = InputHint;
    wmh.input = False;
    XSetWMHints(dpy, win, &wmh);
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
    f_key = XftFontOpenName(dpy, scr, "DejaVu Sans:size=15:bold");
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr), &wc, &c_fg);
    build_keys();

    for (;;) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == Expose) {
            draw();
        } else if (e.type == ClientMessage) {
            if ((Atom)e.xclient.data.l[0] == wm_del)
                return 0;
        } else if (e.type == ButtonPress) {
            for (size_t i = 0; i < keys.size(); i++) {
                const Key &k = keys[i];
                if (e.xbutton.x < k.x || e.xbutton.x >= k.x + k.w ||
                    e.xbutton.y < k.y || e.xbutton.y >= k.y + k.h)
                    continue;
                if (k.cmd == 0 || k.cmd == 4) {
                    send_char(k.ch);
                    if (shift && k.cmd == 0) {   // Shift на одну букву
                        shift = 0;
                        build_keys();
                        draw();
                    }
                } else if (k.cmd == 1) {
                    shift = !shift;
                    build_keys();
                    draw();
                } else if (k.cmd == 2) {
                    send_keysym(XK_BackSpace);
                } else if (k.cmd == 3) {
                    layer = (layer + 1) % 3;
                    shift = 0;
                    build_keys();
                    draw();
                } else if (k.cmd == 5) {
                    send_keysym(XK_Return);
                } else if (k.cmd == 6) {
                    return 0;
                }
                break;
            }
        }
    }
    return 0;
}
