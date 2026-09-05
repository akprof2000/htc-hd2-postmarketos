// Статусная полоска IceMobile — нативная замена прежней на Python/Tk.
//
// Слева: уровень сотовой сети, тип сети, Wi-Fi. Центр: заголовок
// активного окна. Справа: значки событий (SMS, пропущенные, будильник,
// фонарик), кнопка закрытия окна и заряд.
// Тап по полоске — шторка, тап по красному крестику — закрыть окно.
//
// Сборка: g++ -O2 statusbar.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o statusbar

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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>

/* Строка состояния вдвое выше прежней: на этом экране 24 точки читались
 * с трудом. Высота задана здесь одним числом, всё остальное считается от
 * него — базовая линия текста берётся из метрик шрифта. */
static const int W = 480, H = 48;
static int base_y = H / 2;              /* базовая линия, см. setup */
static const unsigned long BG = 0x000000, RED = 0xa4262c;

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_small, *f_bold, *f_sym, *f_title;
static XftColor c_fg, c_dim, c_red, c_ok;

static std::string title;              // заголовок активного окна
static Window active = 0;              // и оно само — для крестика
static int have_close = 0;

// ── чтение системы ───────────────────────────────────────────────────
static std::string readf(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return "";
    char b[512];
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0)
        return "";
    b[n] = 0;
    std::string s(b);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    return s;
}

static int count_lines(const char *path)
{
    std::string s = readf(path);
    if (s.empty())
        return 0;
    int n = 1;
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '\n')
            n++;
    return n;
}

static std::string capture(const char *cmd)
{
    FILE *f = popen(cmd, "r");
    if (!f)
        return "";
    std::string out;
    char b[256];
    while (fgets(b, sizeof(b), f))
        out += b;
    pclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

static void sh(const char *cmd)
{
    pid_t p = fork();
    if (p != 0)
        return;
    setsid();
    if (fork() != 0)
        _exit(0);
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
}

// ── активное окно: спрашиваем X напрямую, без xdotool ────────────────
static Window get_active(void)
{
    Atom prop = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
    if (prop == None)
        return 0;
    Atom type;
    int fmt;
    unsigned long n, rest;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, RootWindow(dpy, scr), prop, 0, 1, False,
                           XA_WINDOW, &type, &fmt, &n, &rest, &data)
        != Success || !data)
        return 0;
    Window w = *(Window *)data;
    XFree(data);
    return w;
}

static std::string win_name(Window w)
{
    if (!w)
        return "";
    // сначала UTF-8 имя (_NET_WM_NAME), потом обычное
    Atom p = XInternAtom(dpy, "_NET_WM_NAME", True);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", True);
    Atom type;
    int fmt;
    unsigned long n, rest;
    unsigned char *data = NULL;
    if (p != None && utf8 != None &&
        XGetWindowProperty(dpy, w, p, 0, 64, False, utf8, &type, &fmt, &n,
                           &rest, &data) == Success && data) {
        std::string s((char *)data);
        XFree(data);
        if (!s.empty())
            return s;
    }
    char *nm = NULL;
    if (XFetchName(dpy, w, &nm) && nm) {
        std::string s(nm);
        XFree(nm);
        return s;
    }
    return "";
}

// ── отрисовка ────────────────────────────────────────────────────────
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

static int wifi_up = 0, alarm_set = 0;
static const int CLOSE_W = 60;
static int close_x = 0;

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);

    // слева: уровень сети полосками, тип сети, Wi-Fi
    std::string csq = readf("/run/phone/csq");
    int bars = csq.empty() ? 0 : atoi(csq.c_str());
    if (bars > 5) bars = 5;
    std::string left;
    static const char *B[] = {"▁", "▂", "▃", "▄", "▅"};
    for (int i = 0; i < 5; i++)
        left += (i < bars) ? B[i] : "·";
    std::string net = readf("/run/phone/net");
    size_t sp = net.find(' ');
    std::string nettype = (sp == std::string::npos) ? net : net.substr(0, sp);
    if (title.empty()) {               // без окна показываем и тип сети
        if (!nettype.empty())
            left += " " + nettype;
        if (wifi_up)
            left += " W";
    }
    text(f_small, &c_dim, 6, base_y, left.c_str());
    int lx = 12 + tw(f_small, left.c_str());

    // справа: заряд
    std::string cap = readf("/sys/class/power_supply/battery/capacity");
    std::string chg = readf("/sys/class/power_supply/battery/status");
    std::string bat = ((chg == "Charging" || chg == "Full") ? "⚡" : "")
                      + cap + "%";
    int low = !cap.empty() && atoi(cap.c_str()) <= 20 && chg == "Discharging";
    int bx = W - 8 - tw(f_bold, bat.c_str());
    text(f_bold, low ? &c_red : &c_dim, bx, base_y, bat.c_str());

    // кнопка закрытия активного окна
    have_close = !title.empty();
    close_x = bx - CLOSE_W - 4;
    if (have_close) {
        XSetForeground(dpy, gc, RED);
        XFillRectangle(dpy, buf, gc, close_x, 4, CLOSE_W, H - 8);
        const char *x = "✕";
        text(f_bold, &c_fg, close_x + (CLOSE_W - tw(f_bold, x)) / 2, base_y, x);
    }

    // значки событий
    std::string ev;
    int sms = count_lines("/run/phone/sms_new");
    char b[64];
    if (sms) {
        snprintf(b, sizeof(b), "✉%d  ", sms);
        ev += b;
    }
    std::string missed = readf("/run/phone/missed");
    if (!missed.empty() && missed != "0")
        ev += "☎" + missed + "  ";
    if (alarm_set)
        ev += "⏰  ";
    std::string torch = readf("/sys/class/leds/flashlight/brightness");
    if (!torch.empty() && torch != "0")
        ev += "☀";
    int ex = (have_close ? close_x : bx) - 6 - tw(f_sym, ev.c_str());
    if (!ev.empty())
        text(f_sym, &c_ok, ex, base_y, ev.c_str());

    // заголовок активного окна — по центру оставшегося места
    if (!title.empty()) {
        std::string t = title;
        int room = ex - lx - 8;
        while (!t.empty() && tw(f_title, t.c_str()) > room)
            t.erase(t.size() - 1);
        text(f_title, &c_fg, lx + 6, base_y, t.c_str());
    }

    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

// Окно, чьё имя мы спрашиваем, может закрыться между запросами — Xlib
// на это отвечает ошибкой и по умолчанию завершает программу. Полоска
// от этого падала; ошибки просто игнорируем.
static int x_error(Display *d, XErrorEvent *e)
{
    (void)d; (void)e;
    return 0;
}

int main(void)
{
    // O_CLOEXEC обязателен: без него запущенные нами программы
    // наследуют эту блокировку и держат её после нашего выхода —
    // тогда следующий экземпляр уже не стартует (так шторка
    // держала блокировку статус-полоски)
    int lock = open("/run/.statusbar.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "нет доступа к X\n");
        return 1;
    }
    XSetErrorHandler(x_error);
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "statusbar");
    XClassHint ch;
    ch.res_name = (char *)"statusbar";
    ch.res_class = (char *)"Statusbar";
    XSetClassHint(dpy, win, &ch);
    // InputHint=False СТАВИТЬ НЕЛЬЗЯ: IceWM тогда не доставляет полоске
    // нажатия вовсе (проверено — ни крестик, ни шторка не отзывались).
    // Подмену цели крестика решаем иначе: номер окна запоминается только
    // для настоящего приложения, см. ниже.
    XSizeHints sh_;
    sh_.flags = PPosition | PSize | PMinSize | PMaxSize;
    sh_.x = 0; sh_.y = 0;
    sh_.width = sh_.min_width = sh_.max_width = W;
    sh_.height = sh_.min_height = sh_.max_height = H;
    XSetWMNormalHints(dpy, win, &sh_);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask |
                 ButtonReleaseMask | Button1MotionMask);
    XMapWindow(dpy, win);

    buf = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, buf, 0, NULL);
    xd = XftDrawCreate(dpy, buf, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=15");
    f_bold = XftFontOpenName(dpy, scr, "DejaVu Sans:size=15:bold");
    f_sym = XftFontOpenName(dpy, scr, "DejaVu Sans:size=16");
    /* заголовку — шрифт помельче: рядом с крупным зарядом и крестиком
     * длинные названия («Калькулятор», «Контакты») иначе не влезают */
    f_title = XftFontOpenName(dpy, scr, "DejaVu Sans:size=12:bold");
    /* базовая линия — по метрикам шрифта, чтобы текст стоял ровно
     * посередине полосы при любой её высоте */
    if (f_bold)
        base_y = (H + f_bold->ascent - f_bold->descent) / 2;
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x9a9a, 0xa4a4, 0xb0b0, 0xffff};
    XRenderColor rc = {0xefef, 0x5353, 0x5050, 0xffff};
    XRenderColor okc = {0x6666, 0xbbbb, 0x6a6a, 0xffff};
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);
    XftColorAllocValue(dpy, vis, cm, &rc, &c_red);
    XftColorAllocValue(dpy, vis, cm, &okc, &c_ok);

    int xfd = ConnectionNumber(dpy);
    time_t last_slow = 0;
    int swipe_from = 0, swipe_done = 0;
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose) {
                draw();
            } else if (e.type == ButtonPress) {
                swipe_from = e.xbutton.y_root;
                swipe_done = 0;
                if (have_close && e.xbutton.x >= close_x &&
                    e.xbutton.x < close_x + CLOSE_W) {
                    swipe_done = 1;    // это нажатие на крестик, не свайп
                    // именно по номеру окна: к моменту нажатия активной
                    // становится сама полоска, и :ACTIVE: закрыл бы её
                    if (active) {
                        char cmd[96];
                        snprintf(cmd, sizeof(cmd),
                                 "DISPLAY=:0 wmctrl -i -c 0x%lx",
                                 (unsigned long)active);
                        sh(cmd);
                    }
                }
            } else if (e.type == MotionNotify) {
                // шторка открывается протягиванием вниз, а не тапом:
                // случайное касание полоски больше её не дёргает
                if (!swipe_done && (e.xmotion.state & Button1Mask) &&
                    e.xmotion.y_root - swipe_from > 20) {
                    swipe_done = 1;
                    sh("DISPLAY=:0 /usr/local/bin/shade");
                }
            }
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {1, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);

        // активное окно и его имя — дёшево, прямо у X
        Window a = get_active();
        std::string t = win_name(a);
        // Rockbox рисует ровно во весь экран, и его верхняя строка
        // оказывалась под нами. Двигать его окно не дают ни оконный
        // менеджер, ни SDL, поэтому уступаем место сами: пока плеер
        // впереди, строку состояния прячем, свои часы и заряд он
        // показывает сам.
        {
            static int hidden = 0;
            int want_hide = (t == "Rockbox");
            if (want_hide && !hidden) {
                XUnmapWindow(dpy, win);
                hidden = 1;
            } else if (!want_hide && hidden) {
                XMapRaised(dpy, win);
                hidden = 0;
            }
        }
        if (t == "statusbar" || t == "Шторка" || t == "rukbd" ||
            t == "volosd")
            t = title;                 // служебные окна заголовок не меняют
        else if (t == "Домой")
            t = "";
        // номер окна запоминаем ТОЛЬКО для настоящего приложения:
        // служебные окна не должны становиться целью крестика
        if (!t.empty() && t != title) {
            active = a;
            title = t;
        } else if (t.empty() && win_name(a) == "Домой") {
            active = 0;
            title = "";
        }
        // Запомненное окно могло уже закрыться — тогда крестику нечего
        // закрывать, и он обязан исчезнуть. Раньше он оставался висеть,
        // если после закрытия фокус уходил на служебное окно.
        if (active) {
            XWindowAttributes wa;
            int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(x_error);
            int ok = XGetWindowAttributes(dpy, active, &wa) &&
                     wa.map_state == IsViewable;
            XSetErrorHandler(old);
            if (!ok) {
                active = 0;
                title = "";
            }
        }
        // медленные проверки — раз в 30 с
        time_t now = time(NULL);
        if (now - last_slow >= 30) {
            last_slow = now;
            wifi_up = capture("iw dev wlan0 link 2>/dev/null")
                          .find("Connected") != std::string::npos;
            alarm_set = capture("crontab -l 2>/dev/null")
                            .find("icemobile-alarm") != std::string::npos;
        }
        XRaiseWindow(dpy, win);        // окна WM иначе накрывают полоску
        draw();
    }
    return 0;
}
