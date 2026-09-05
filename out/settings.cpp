// Настройки HTC HD2 — нативная замена settings на Python/Tk.
//
// Яркость, громкость, выбор сети, сброс значков уведомлений и живое
// состояние телефона. Обновляется раз в две секунды.
//
// Сборка: g++ -O2 settings.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o settings

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

static const int W = 480, H = 752, WIN_Y = 48;
static const unsigned long BG = 0x101828, KEY = 0x2a3a55, ACC = 0x1f7a33,
                           INFO = 0x1c2a40;
static const char *BL = "/sys/class/leds/lcd-backlight/brightness";

static const int HEAD_Y = 40;
static const int STEP_Y = 70, STEP_H = 62, STEP_GAP = 10;
static const int TILE_Y = 214, TILE_H = 64, TILE_GAP = 8;
static const int INFO_LBL_Y = 452, INFO_Y = 466, INFO_H = 130;

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_head, *f_val, *f_sign, *f_tile, *f_info;
static XftColor c_fg, c_dim;

static int brightness = 180, volume = 70;
static int tile_lit = -1;               // подсветка нажатой плитки
static double tile_at = 0;

struct Tile { const char *label; const char *at_cmd; };
static const Tile TILES[3] = {
    {"Сеть 2G — звонки", "AT+COPS=1,2,\"25002\",0"},
    {"Сеть 3G — интернет", "AT+COPS=0"},
    {"Сбросить значки уведомлений", NULL},
};

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static std::string readf(const char *p)
{
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return "";
    char b[4096];
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

static void writef(const char *p, const char *v)
{
    int fd = open(p, O_WRONLY | O_TRUNC);
    if (fd < 0)
        return;
    if (write(fd, v, strlen(v)) < 0) { }
    close(fd);
}

static void at(const char *cmd)
{
    int fd = open("/run/phone/cmd", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return;
    std::string s = std::string(cmd) + "\n";
    if (write(fd, s.c_str(), s.size()) < 0) { }
    close(fd);
}

static void buzz(void)
{
    writef("/sys/class/timed_output/vibrator/enable", "30");
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

static std::vector<std::string> info_lines;

static void collect(void)
{
    info_lines.clear();
    char b[192];
    std::string net = readf("/run/phone/net"), dbm = readf("/run/phone/dbm");
    snprintf(b, sizeof(b), "Сеть: %s · %s дБм",
             net.empty() ? "—" : net.c_str(), dbm.empty() ? "?" : dbm.c_str());
    info_lines.push_back(b);

    std::string st = readf("/sys/class/power_supply/battery/status");
    const char *ru = "?";
    if (st == "Charging") ru = "заряжается";
    else if (st == "Full") ru = "заряжена";
    else if (st == "Discharging") ru = "разряжается";
    snprintf(b, sizeof(b), "Батарея: %s%% (%s)",
             readf("/sys/class/power_supply/battery/capacity").c_str(), ru);
    info_lines.push_back(b);

    std::string mi = readf("/proc/meminfo");
    long fr = 0, cache = 0;
    size_t p = 0;
    while (p < mi.size()) {
        size_t e = mi.find('\n', p);
        if (e == std::string::npos)
            e = mi.size();
        std::string ln = mi.substr(p, e - p);
        p = e + 1;
        long v = 0;
        if (sscanf(ln.c_str(), "MemFree: %ld", &v) == 1) fr = v;
        else if (sscanf(ln.c_str(), "Cached: %ld", &v) == 1) cache = v;
    }
    snprintf(b, sizeof(b), "Память: свободно %ld МБ", (fr + cache) / 1024);
    info_lines.push_back(b);

    long up = atol(readf("/proc/uptime").c_str());
    snprintf(b, sizeof(b), "Работает: %ld ч %02ld мин", up / 3600,
             up % 3600 / 60);
    info_lines.push_back(b);
}

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);
    const char *t = "Настройки";
    text(f_head, &c_fg, (W - tw(f_head, t)) / 2, HEAD_Y, t);

    char b[64];
    for (int i = 0; i < 2; i++) {
        int y = STEP_Y + i * (STEP_H + STEP_GAP);
        XSetForeground(dpy, gc, KEY);
        XFillRectangle(dpy, buf, gc, 12, y, 68, STEP_H);
        XFillRectangle(dpy, buf, gc, W - 80, y, 68, STEP_H);
        text(f_sign, &c_fg, 12 + (68 - tw(f_sign, "−")) / 2,
             y + STEP_H / 2 + 10, "−");
        text(f_sign, &c_fg, W - 80 + (68 - tw(f_sign, "+")) / 2,
             y + STEP_H / 2 + 10, "+");
        if (i == 0)
            snprintf(b, sizeof(b), "Яркость: %d%%",
                     (brightness * 100 + 127) / 255);
        else
            snprintf(b, sizeof(b), "Громкость: %d%%", volume);
        text(f_val, &c_fg, (W - tw(f_val, b)) / 2, y + STEP_H / 2 + 7, b);
    }

    for (int i = 0; i < 3; i++) {
        int y = TILE_Y + i * (TILE_H + TILE_GAP);
        XSetForeground(dpy, gc, i == tile_lit ? ACC : KEY);
        XFillRectangle(dpy, buf, gc, 12, y, W - 24, TILE_H);
        text(f_tile, &c_fg, (W - tw(f_tile, TILES[i].label)) / 2,
             y + TILE_H / 2 + 7, TILES[i].label);
    }

    text(f_tile, &c_dim, 14, INFO_LBL_Y, "Состояние");
    XSetForeground(dpy, gc, INFO);
    XFillRectangle(dpy, buf, gc, 12, INFO_Y, W - 24, INFO_H);
    for (size_t i = 0; i < info_lines.size(); i++)
        text(f_info, &c_fg, 26, INFO_Y + 26 + (int)i * 28,
             info_lines[i].c_str());

    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void click(int x, int y)
{
    for (int i = 0; i < 2; i++) {
        int sy = STEP_Y + i * (STEP_H + STEP_GAP);
        if (y < sy || y >= sy + STEP_H)
            continue;
        int d = 0;
        if (x < 80) d = -1;
        else if (x > W - 80) d = 1;
        if (!d)
            return;
        buzz();
        if (i == 0) {
            brightness += d * 35;
            if (brightness < 10) brightness = 10;
            if (brightness > 255) brightness = 255;
            char b[8];
            snprintf(b, sizeof(b), "%d", brightness);
            writef(BL, b);
        } else {
            volume += d * 10;
            if (volume < 0) volume = 0;
            if (volume > 100) volume = 100;
            char b[24];
            snprintf(b, sizeof(b), "@vol %d", volume);
            at(b);
            snprintf(b, sizeof(b), "%d", volume);
            writef("/run/phone/vol", b);
        }
        return;
    }
    for (int i = 0; i < 3; i++) {
        int ty = TILE_Y + i * (TILE_H + TILE_GAP);
        if (y < ty || y >= ty + TILE_H)
            continue;
        buzz();
        tile_lit = i;
        tile_at = now_s();
        if (TILES[i].at_cmd)
            at(TILES[i].at_cmd);
        else                                // сброс значков уведомлений
            for (const char *f : {"/run/phone/sms_new", "/run/phone/missed"}) {
                int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0)
                    close(fd);
            }
        return;
    }
}

int main(void)
{
    int lock = open("/run/.settings.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Настройки");
    XClassHint ch;
    ch.res_name = (char *)"settingsapp";
    ch.res_class = (char *)"Settingsapp";
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
    f_head = XftFontOpenName(dpy, scr, "DejaVu Sans:size=18:bold");
    f_val = XftFontOpenName(dpy, scr, "DejaVu Sans:size=14:bold");
    f_sign = XftFontOpenName(dpy, scr, "DejaVu Sans:size=22:bold");
    f_tile = XftFontOpenName(dpy, scr, "DejaVu Sans:size=12:bold");
    f_info = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);

    std::string b = readf(BL);
    if (!b.empty())
        brightness = atoi(b.c_str());
    std::string v = readf("/run/phone/vol");
    if (!v.empty())
        volume = atoi(v.c_str());
    collect();

    int xfd = ConnectionNumber(dpy);
    double last = 0;
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
        struct timeval tv = {0, 300000};
        select(xfd + 1, &fds, NULL, NULL, &tv);
        double t = now_s();
        if (tile_lit >= 0 && t - tile_at > 0.6) {
            tile_lit = -1;
            draw();
        }
        if (t - last > 2.0) {
            last = t;
            collect();
            draw();
        }
    }
    return 0;
}
