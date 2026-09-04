// Часы HTC HD2 — нативная замена clock-app на Python/Tk.
//
// Три вкладки: будильник, таймер, секундомер. Будильник живёт в crontab
// (busybox crond), поэтому срабатывает и с закрытым приложением.
//
// Сборка: g++ -O2 clock.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o clock

#include <X11/Xlib.h>
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

#include <string>
#include <vector>

static const int W = 480, H = 776, WIN_Y = 24;
static const unsigned long BG = 0x101828, KEY = 0x1c2a40, ACC = 0x1f7a33,
                           RED = 0xa4262c;
static const char *TAG = "# icemobile-alarm";

static const int TAB_Y = 8, TAB_H = 56;
static const int BIG_Y = 190, STATE_Y = 236;
static const int ADJ_Y = 280, ADJ_H = 72;
static const int ACT_Y = 392, ACT_H = 96;

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_big, *f_tab, *f_btn, *f_small;
static XftColor c_fg, c_dim, c_ok;

static int tab = 0;                                  // 0 будильник 1 таймер 2 сек
static const char *TABS[3] = {"Будильник", "Таймер", "Секунд."};

static int al_h = 7, al_m = 0, al_on = 0;
static int t_left = 300, t_run = 0;
static double sw_acc = 0;
static double sw_start = -1;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static std::string capture(const char *cmd)
{
    FILE *f = popen(cmd, "r");
    if (!f)
        return "";
    std::string o;
    char b[256];
    while (fgets(b, sizeof(b), f))
        o += b;
    pclose(f);
    return o;
}

// ── будильник в crontab ──────────────────────────────────────────────
static void cron_load(void)
{
    std::string out = capture("crontab -l 2>/dev/null");
    size_t p = 0;
    while (p < out.size()) {
        size_t e = out.find('\n', p);
        if (e == std::string::npos)
            e = out.size();
        std::string ln = out.substr(p, e - p);
        p = e + 1;
        if (ln.find(TAG) == std::string::npos)
            continue;
        int m = 0, h = 0;
        if (sscanf(ln.c_str(), "%d %d", &m, &h) == 2) {
            al_h = h;
            al_m = m;
            al_on = 1;
        }
    }
}

static void cron_save(void)
{
    std::string out = capture("crontab -l 2>/dev/null");
    std::string keep;
    size_t p = 0;
    while (p < out.size()) {
        size_t e = out.find('\n', p);
        if (e == std::string::npos)
            e = out.size();
        std::string ln = out.substr(p, e - p);
        p = e + 1;
        if (!ln.empty() && ln.find(TAG) == std::string::npos)
            keep += ln + "\n";
    }
    if (al_on) {
        char b[160];
        snprintf(b, sizeof(b), "%d %d * * * /usr/local/bin/alarm-fire %s\n",
                 al_m, al_h, TAG);
        keep += b;
    }
    FILE *f = popen("crontab - 2>/dev/null", "w");
    if (f) {
        fwrite(keep.c_str(), 1, keep.size(), f);
        pclose(f);
    }
}

static void ring(void)
{
    if (fork() == 0) {
        setsid();
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 1); dup2(null, 2); }
        execl("/bin/sh", "sh", "-c",
              "/usr/local/bin/ringtone & "
              "echo 600 > /sys/class/timed_output/vibrator/enable",
              (char *)NULL);
        _exit(1);
    }
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

static void centered(XftFont *fn, XftColor *c, int y, const char *s)
{
    text(fn, c, (W - tw(fn, s)) / 2, y, s);
}

// подпись кнопки по центру прямоугольника
static void button(int x, int y, int w, int h, unsigned long col,
                   XftFont *fn, const char *label)
{
    XSetForeground(dpy, gc, col);
    XFillRectangle(dpy, buf, gc, x, y, w, h);
    text(fn, &c_fg, x + (w - tw(fn, label)) / 2, y + h / 2 + 8, label);
}

static double sw_val(void)
{
    double v = sw_acc;
    if (sw_start >= 0)
        v += now_s() - sw_start;
    return v;
}

struct Adj { const char *label; int dh, dm; };
static const Adj ALARM_ADJ[4] = {{"−1 ч", -1, 0}, {"−10 м", 0, -10},
                                 {"+10 м", 0, 10}, {"+1 ч", 1, 0}};
struct TAdd { const char *label; int sec; };
static const TAdd TIMER_ADD[4] = {{"+1 м", 60}, {"+5 м", 300},
                                  {"+10 м", 600}, {"Сброс", 0}};

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);

    int tabw = (W - 16) / 3;
    for (int i = 0; i < 3; i++)
        button(8 + i * tabw, TAB_Y, tabw - 4, TAB_H, i == tab ? ACC : KEY,
               f_tab, TABS[i]);

    char b[64];
    int aw = (W - 16) / 4;
    if (tab == 0) {
        snprintf(b, sizeof(b), "%02d:%02d", al_h, al_m);
        centered(f_big, &c_fg, BIG_Y, b);
        centered(f_small, al_on ? &c_ok : &c_dim, STATE_Y,
                 al_on ? "будильник включён" : "выключен");
        for (int i = 0; i < 4; i++)
            button(8 + i * aw, ADJ_Y, aw - 4, ADJ_H, KEY, f_small,
                   ALARM_ADJ[i].label);
        button(24, ACT_Y, W - 48, ACT_H, al_on ? RED : ACC, f_btn,
               al_on ? "Выключить" : "Включить");
    } else if (tab == 1) {
        snprintf(b, sizeof(b), "%02d:%02d", t_left / 60, t_left % 60);
        centered(f_big, &c_fg, BIG_Y, b);
        for (int i = 0; i < 4; i++)
            button(8 + i * aw, ADJ_Y, aw - 4, ADJ_H, KEY, f_small,
                   TIMER_ADD[i].label);
        button(24, ACT_Y, W - 48, ACT_H, t_run ? RED : ACC, f_btn,
               t_run ? "Пауза" : "Старт");
    } else {
        double v = sw_val();
        snprintf(b, sizeof(b), "%02d:%02d.%d", (int)v / 60, (int)v % 60,
                 (int)(v * 10) % 10);
        centered(f_big, &c_fg, BIG_Y, b);
        int hw = (W - 56) / 2;
        button(24, ACT_Y, hw, ACT_H, sw_start >= 0 ? RED : ACC, f_btn,
               sw_start >= 0 ? "Стоп" : "Старт");
        button(24 + hw + 8, ACT_Y, hw, ACT_H, KEY, f_btn, "Сброс");
    }
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void click(int x, int y)
{
    int tabw = (W - 16) / 3;
    if (y >= TAB_Y && y < TAB_Y + TAB_H) {
        int i = (x - 8) / tabw;
        if (i >= 0 && i < 3)
            tab = i;
        return;
    }
    int aw = (W - 16) / 4;
    if (tab != 2 && y >= ADJ_Y && y < ADJ_Y + ADJ_H) {
        int i = (x - 8) / aw;
        if (i < 0 || i > 3)
            return;
        if (tab == 0) {
            int m = (al_h * 60 + al_m + ALARM_ADJ[i].dh * 60 +
                     ALARM_ADJ[i].dm + 24 * 60) % (24 * 60);
            al_h = m / 60;
            al_m = m % 60;
            if (al_on)
                cron_save();
        } else if (TIMER_ADD[i].sec == 0) {
            t_left = 0;
            t_run = 0;
        } else
            t_left += TIMER_ADD[i].sec;
        return;
    }
    if (y >= ACT_Y && y < ACT_Y + ACT_H) {
        if (tab == 0) {
            al_on = !al_on;
            cron_save();
        } else if (tab == 1) {
            if (t_left > 0)
                t_run = !t_run;
        } else {
            int hw = (W - 56) / 2;
            if (x < 24 + hw) {                 // старт/стоп
                if (sw_start < 0)
                    sw_start = now_s();
                else {
                    sw_acc += now_s() - sw_start;
                    sw_start = -1;
                }
            } else {                            // сброс
                sw_acc = 0;
                sw_start = -1;
            }
        }
    }
}

int main(void)
{
    int lock = open("/run/.clockapp.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Часы");
    XClassHint ch;
    ch.res_name = (char *)"clockapp";
    ch.res_class = (char *)"Clockapp";
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
    f_big = XftFontOpenName(dpy, scr, "DejaVu Sans:size=44:bold");
    f_tab = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11:bold");
    f_btn = XftFontOpenName(dpy, scr, "DejaVu Sans:size=17:bold");
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=13:bold");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XRenderColor okc = {0x6666, 0xbbbb, 0x6a6a, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);
    XftColorAllocValue(dpy, vis, cm, &okc, &c_ok);

    cron_load();
    int xfd = ConnectionNumber(dpy);
    double last_sec = now_s();
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
                click(e.xbutton.x, e.xbutton.y);
                draw();
            }
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        // секундомер обновляем чаще, чтобы десятые не стояли
        struct timeval tv = {0, tab == 2 ? 100000 : 400000};
        select(xfd + 1, &fds, NULL, NULL, &tv);
        double t = now_s();
        if (t - last_sec >= 1.0) {
            last_sec = t;
            if (t_run && --t_left <= 0) {
                t_left = 0;
                t_run = 0;
                ring();
            }
            draw();
        } else if (tab == 2 && sw_start >= 0)
            draw();
    }
    return 0;
}
