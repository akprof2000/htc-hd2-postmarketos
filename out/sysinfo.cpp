// Сведения о системе HTC HD2 — нативная замена sysinfo на Python/Tk.
//
// Модель, ядро, время работы, батарея, память, диск, сотовая сеть, Wi-Fi.
// Обновляется раз в две секунды.
//
// Сборка: g++ -O2 sysinfo.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o sysinfo

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

static const int W = 480, H = 752, WIN_Y = 48;
static const unsigned long BG = 0x101828, ROW = 0x1c2a40;

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_head, *f_key, *f_val;
static XftColor c_fg, c_dim;

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
    while (!o.empty() && (o.back() == '\n' || o.back() == ' '))
        o.pop_back();
    return o;
}

struct Row { std::string key, val; };
static std::vector<Row> rows;

static void collect(void)
{
    rows.clear();
    rows.push_back({"Модель", "HTC HD2 (Leo) · pmOS"});
    struct utsname u;
    if (uname(&u) == 0)
        rows.push_back({"Ядро", u.release});

    std::string up = readf("/proc/uptime");
    long s = atol(up.c_str());
    char b[128];
    snprintf(b, sizeof(b), "%ld ч %02ld мин", s / 3600, s % 3600 / 60);
    rows.push_back({"Работает", b});

    std::string cap = readf("/sys/class/power_supply/battery/capacity");
    std::string st = readf("/sys/class/power_supply/battery/status");
    const char *ru = "?";                // ядро отвечает по-английски
    if (st == "Charging") ru = "заряжается";
    else if (st == "Full") ru = "заряжена";
    else if (st == "Discharging") ru = "разряжается";
    else if (st == "Not charging") ru = "не заряжается";
    rows.push_back({"Батарея", cap + "% · " + ru});

    // память: свободно = MemFree + Buffers + Cached
    std::string mi = readf("/proc/meminfo");
    long total = 0, freeb = 0, bufs = 0, cache = 0;
    size_t p = 0;
    while (p < mi.size()) {
        size_t e = mi.find('\n', p);
        if (e == std::string::npos)
            e = mi.size();
        std::string ln = mi.substr(p, e - p);
        p = e + 1;
        long v = atol(ln.c_str() + ln.find(':') + 1);
        if (!ln.compare(0, 9, "MemTotal:")) total = v;
        else if (!ln.compare(0, 8, "MemFree:")) freeb = v;
        else if (!ln.compare(0, 8, "Buffers:")) bufs = v;
        else if (!ln.compare(0, 7, "Cached:")) cache = v;
    }
    snprintf(b, sizeof(b), "свободно %ld из %ld МБ",
             (freeb + bufs + cache) / 1024, total / 1024);
    rows.push_back({"Память", b});

    std::string df = capture("df -h / | tail -1");
    if (!df.empty()) {
        char size[32] = "", used[32] = "";
        sscanf(df.c_str(), "%*s %31s %31s", size, used);
        rows.push_back({"Диск", std::string("занято ") + used + " из " + size});
    }

    std::string net = readf("/run/phone/net"), dbm = readf("/run/phone/dbm");
    if (!net.empty())
        rows.push_back({"Сотовая сеть", net + (dbm.empty() ? "" :
                                               " · " + dbm + " дБм")});
    std::string wifi = capture("iw dev wlan0 link 2>/dev/null | head -1");
    if (wifi.find("Connected") != std::string::npos) {
        std::string ssid = capture("iw dev wlan0 link 2>/dev/null | "
                                   "awk '/SSID/{print $2}'");
        rows.push_back({"Wi-Fi", ssid.empty() ? "подключён" : ssid});
    } else
        rows.push_back({"Wi-Fi", "нет"});

    std::string ip = capture("ip -4 addr show wlan0 2>/dev/null | "
                             "awk '/inet /{print $2}'");
    if (!ip.empty())
        rows.push_back({"Адрес", ip});

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(b, sizeof(b), "%H:%M:%S · %d.%m.%Y", &tm);
    rows.push_back({"Время", b});
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
    const char *t = "Система";
    text(f_head, &c_fg, (W - tw(f_head, t)) / 2, 44, t);
    for (size_t i = 0; i < rows.size(); i++) {
        int y = 70 + (int)i * 58;
        if (y > H - 40)
            break;
        XSetForeground(dpy, gc, ROW);
        XFillRectangle(dpy, buf, gc, 10, y, W - 20, 52);
        text(f_key, &c_dim, 20, y + 21, rows[i].key.c_str());
        // значение может быть длинным — обрезаем по ширине окна
        std::string v = rows[i].val;
        while (!v.empty() && tw(f_val, v.c_str()) > W - 44)
            v.erase(v.size() - 1);
        text(f_val, &c_fg, 20, y + 43, v.c_str());
    }
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

int main(void)
{
    int lock = open("/run/.sysinfo.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Система");
    XClassHint ch;
    ch.res_name = (char *)"sysinfo";
    ch.res_class = (char *)"Sysinfo";
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
    f_head = XftFontOpenName(dpy, scr, "DejaVu Sans:size=17:bold");
    f_key = XftFontOpenName(dpy, scr, "DejaVu Sans:size=10");
    f_val = XftFontOpenName(dpy, scr, "DejaVu Sans:size=13:bold");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);

    collect();
    int xfd = ConnectionNumber(dpy);
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose)
                draw();
            else if (e.type == ClientMessage &&
                     (Atom)e.xclient.data.l[0] == wm_del)
                return 0;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {2, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);
        collect();
        draw();
    }
    return 0;
}
