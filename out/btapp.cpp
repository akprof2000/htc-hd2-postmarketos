// Bluetooth HTC HD2 — нативная замена btapp на Python/Tk.
//
// Включение (btsetup заливает прошивку, ~20 с), видимость, поиск своим
// сканером btscan (hcitool на этом чипе не работает), сопряжение и
// отправка снимка. Долгие команды крутятся в отдельном процессе, вывод
// забираем из файла — интерфейс при этом не замирает.
//
// Сборка: g++ -O2 btapp.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o btapp

#include <X11/Xlib.h>
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
#include <vector>

static const int W = 480, H = 776, WIN_Y = 24;
static const unsigned long BG = 0x101828, KEY = 0x22314a, ACC = 0x1f7a33,
                           ROW = 0x182238, BUSY = 0x8a5a2e;

static const int HEAD_Y = 40, ST_Y = 66, ST_Y2 = 88;
static const int TILE_Y = 104, TILE_H = 64;
static const int SCAN_Y = 176, SCAN_H = 64;
static const int LBL_Y = 262, LIST_Y = 276, DEV_H = 104, DEV_GAP = 6;
static const int JOB_OUT_MAX = 16384;

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_head, *f_st, *f_tile, *f_dev, *f_small;
static XftColor c_fg, c_dim;

struct Dev { std::string name, addr, rssi; };
static std::vector<Dev> devs;
static std::string st1 = "проверяю…", st2;
static int bt_on = 0, bt_vis = 0;
static std::string bt_addr;

// одно фоновое дело за раз
static pid_t job = 0;
static int job_kind = 0;                 // 1 питание 2 поиск 3 прочее
static const char *JOB_OUT = "/tmp/.btapp.out";
// пока сообщение о результате свежее, опрос состояния его не затирает
static time_t msg_until = 0;

static std::string readf(const char *p)
{
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return "";
    std::string s;
    char b[4096];
    ssize_t n;
    while ((n = read(fd, b, sizeof(b))) > 0) {
        s.append(b, n);
        if ((int)s.size() > JOB_OUT_MAX)
            break;
    }
    close(fd);
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
    return o;
}

static void buzz(void)
{
    int fd = open("/sys/class/timed_output/vibrator/enable", O_WRONLY);
    if (fd >= 0) {
        if (write(fd, "30", 2) < 0) { }
        close(fd);
    }
}

static void start_job(const char *cmd, int kind)
{
    if (job)
        return;
    unlink(JOB_OUT);
    pid_t p = fork();
    if (p == 0) {
        setsid();
        int out = open(JOB_OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int null = open("/dev/null", O_RDONLY);
        if (out >= 0) { dup2(out, 1); dup2(out, 2); }
        if (null >= 0) dup2(null, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    job = p;
    job_kind = kind;
}

// состояние контроллера
static void poll_state(void)
{
    std::string o = capture("hciconfig hci0 2>/dev/null");
    bt_on = o.find("UP RUNNING") != std::string::npos;
    bt_vis = o.find("ISCAN") != std::string::npos;
    bt_addr.clear();
    size_t p = 0;
    while (p < o.size()) {                 // ищем что-то вида XX:XX:...
        size_t e = o.find_first_of(" \t\n", p);
        if (e == std::string::npos)
            e = o.size();
        std::string t = o.substr(p, e - p);
        int colons = 0;
        for (char c : t)
            if (c == ':')
                colons++;
        if (colons == 5 && t.size() == 17) {
            bt_addr = t;
            break;
        }
        p = e + 1;
    }
    if (job || time(NULL) < msg_until)
        return;
    if (bt_on) {
        st1 = "включён · " + (bt_addr.empty() ? "?" : bt_addr);
        st2 = std::string("имя «HTC HD2» · видимость: ") +
              (bt_vis ? "да" : "нет");
    } else {
        st1 = "выключен";
        st2.clear();
    }
}

// разбор вывода btscan: адрес, уровень, имя
static void parse_scan(const std::string &out)
{
    devs.clear();
    size_t p = 0;
    while (p < out.size()) {
        size_t e = out.find('\n', p);
        if (e == std::string::npos)
            e = out.size();
        std::string ln = out.substr(p, e - p);
        p = e + 1;
        char addr[64], rssi[32], name[192];
        int n = sscanf(ln.c_str(), "%63s %31s %191[^\n]", addr, rssi, name);
        if (n < 2 || strlen(addr) != 17)
            continue;
        std::string nm = n >= 3 ? name : "";
        size_t d = nm.find("дБм");
        if (d != std::string::npos)
            nm.erase(d, 6);
        while (!nm.empty() && (nm.front() == ' ' || nm.front() == '\t'))
            nm.erase(0, 1);
        devs.push_back({nm.empty() ? "(без имени)" : nm, addr, rssi});
    }
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

// строка, обрезанная по ширине окна
static std::string fit(XftFont *fn, std::string s, int width)
{
    while (!s.empty() && tw(fn, s.c_str()) > width)
        s.erase(s.size() - 1);
    return s;
}

static int devs_fit(void) { return (H - LIST_Y - 6) / (DEV_H + DEV_GAP); }

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);
    const char *t = "Bluetooth";
    text(f_head, &c_fg, (W - tw(f_head, t)) / 2, HEAD_Y, t);
    std::string s1 = fit(f_st, st1, W - 24), s2 = fit(f_st, st2, W - 24);
    text(f_st, &c_dim, (W - tw(f_st, s1.c_str())) / 2, ST_Y, s1.c_str());
    if (!s2.empty())
        text(f_st, &c_dim, (W - tw(f_st, s2.c_str())) / 2, ST_Y2, s2.c_str());

    int hw = (W - 26) / 2;
    XSetForeground(dpy, gc, bt_on ? ACC : KEY);
    XFillRectangle(dpy, buf, gc, 10, TILE_Y, hw, TILE_H);
    const char *pw = bt_on ? "Выключить" : "Включить";
    text(f_tile, &c_fg, 10 + (hw - tw(f_tile, pw)) / 2, TILE_Y + 40, pw);
    XSetForeground(dpy, gc, bt_vis ? ACC : KEY);
    XFillRectangle(dpy, buf, gc, 16 + hw, TILE_Y, hw, TILE_H);
    const char *vs = bt_vis ? "Видим" : "Не видим";
    text(f_tile, &c_fg, 16 + hw + (hw - tw(f_tile, vs)) / 2, TILE_Y + 40, vs);

    int scanning = (job && job_kind == 2);
    XSetForeground(dpy, gc, scanning ? BUSY : KEY);
    XFillRectangle(dpy, buf, gc, 10, SCAN_Y, W - 20, SCAN_H);
    const char *sc = scanning ? "ищу…" : "Найти устройства";
    text(f_tile, &c_fg, (W - tw(f_tile, sc)) / 2, SCAN_Y + 40, sc);

    text(f_small, &c_dim, 14, LBL_Y, "Найденные устройства");
    if (devs.empty()) {
        XSetForeground(dpy, gc, ROW);
        XFillRectangle(dpy, buf, gc, 10, LIST_Y, W - 20, 52);
        const char *e = job && job_kind == 2 ? "идёт поиск, 15 секунд…"
                                             : "нажмите «Найти устройства»";
        text(f_dev, &c_fg, 24, LIST_Y + 32, e);
    }
    int fitn = devs_fit();
    for (int i = 0; i < (int)devs.size() && i < fitn; i++) {
        int y = LIST_Y + i * (DEV_H + DEV_GAP);
        XSetForeground(dpy, gc, ROW);
        XFillRectangle(dpy, buf, gc, 10, y, W - 20, DEV_H);
        text(f_dev, &c_fg, 24, y + 26,
             fit(f_dev, devs[i].name, W - 48).c_str());
        std::string sub = devs[i].addr + " · сигнал " + devs[i].rssi + " дБм";
        text(f_small, &c_dim, 24, y + 48, fit(f_small, sub, W - 48).c_str());
        int bw = (W - 44) / 2;
        XSetForeground(dpy, gc, KEY);
        XFillRectangle(dpy, buf, gc, 16, y + 58, bw, 38);
        XFillRectangle(dpy, buf, gc, 24 + bw, y + 58, bw, 38);
        const char *b1 = "Сопрячь", *b2 = "Отправить фото";
        text(f_small, &c_fg, 16 + (bw - tw(f_small, b1)) / 2, y + 83, b1);
        text(f_small, &c_fg, 24 + bw + (bw - tw(f_small, b2)) / 2, y + 83, b2);
    }
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void click(int x, int y)
{
    int hw = (W - 26) / 2;
    if (y >= TILE_Y && y < TILE_Y + TILE_H) {
        buzz();
        if (x < 10 + hw) {
            if (job)
                return;
            if (bt_on) {
                st1 = "выключаю…";
                st2.clear();
                start_job("hciconfig hci0 down; "
                          "echo 0 > /sys/class/rfkill/rfkill0/state", 1);
            } else {
                st1 = "включаю (заливаю прошивку, ~20 с)…";
                st2.clear();
                start_job("/usr/local/bin/btsetup", 1);
            }
        } else {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "hciconfig hci0 %s",
                     bt_vis ? "noscan" : "piscan");
            start_job(cmd, 3);
        }
        return;
    }
    if (y >= SCAN_Y && y < SCAN_Y + SCAN_H) {
        buzz();
        if (job)
            return;
        if (!bt_on) {
            st1 = "сначала включите Bluetooth";
            st2.clear();
            return;
        }
        devs.clear();
        start_job("/usr/local/bin/btscan 15", 2);
        return;
    }
    int fitn = devs_fit();
    for (int i = 0; i < (int)devs.size() && i < fitn; i++) {
        int dy = LIST_Y + i * (DEV_H + DEV_GAP);
        if (y < dy + 58 || y >= dy + 96)
            continue;
        buzz();
        if (job)
            return;
        int bw = (W - 44) / 2;
        char cmd[512];
        if (x < 16 + bw) {
            st1 = "сопрягаюсь с " + devs[i].addr + " (PIN 0000)…";
            st2.clear();
            snprintf(cmd, sizeof(cmd), "/usr/local/bin/btpair %s 0000",
                     devs[i].addr.c_str());
            start_job(cmd, 3);
        } else {
            std::string pic = capture("ls -1 /root/Pictures/*.jpg "
                                      "2>/dev/null | tail -1");
            while (!pic.empty() && (pic.back() == '\n' || pic.back() == ' '))
                pic.pop_back();
            if (pic.empty()) {
                st1 = "нет снимков в /root/Pictures";
                st2.clear();
                return;
            }
            st1 = "отправляю " + pic + "…";
            st2.clear();
            snprintf(cmd, sizeof(cmd), "/usr/local/bin/btsend %s '%s'",
                     devs[i].addr.c_str(), pic.c_str());
            start_job(cmd, 3);
        }
        return;
    }
}

// дело закончилось — разобрать вывод
static void job_done(void)
{
    std::string out = readf(JOB_OUT);
    int kind = job_kind;
    job = 0;
    job_kind = 0;
    if (kind == 2) {
        parse_scan(out);
        if (devs.empty()) {
            st1 = "устройства не найдены — включите видимость";
            st2 = "на другом телефоне и повторите";
            msg_until = time(NULL) + 8;
        }
    } else if (kind == 3) {
        if (out.find("СОПРЯЖЕНО") != std::string::npos)
            st1 = "сопряжено";
        else if (out.find("ПЕРЕДАН") != std::string::npos)
            st1 = "файл передан!";
        else
            st1 = "не получилось — устройство не ответило";
        st2.clear();
        msg_until = time(NULL) + 8;
    }
    poll_state();
}

int main(void)
{
    int lock = open("/tmp/.btapp.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Bluetooth");
    XClassHint ch;
    ch.res_name = (char *)"btapp";
    ch.res_class = (char *)"Btapp";
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
    f_st = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11");
    f_tile = XftFontOpenName(dpy, scr, "DejaVu Sans:size=13:bold");
    f_dev = XftFontOpenName(dpy, scr, "DejaVu Sans:size=12");
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11:bold");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xffff, 0xffff, 0xffff, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);

    poll_state();
    int xfd = ConnectionNumber(dpy);
    time_t last = 0;
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
        struct timeval tv = {1, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);
        if (job && waitpid(job, NULL, WNOHANG) == job) {
            job_done();
            draw();
        }
        if (time(NULL) - last >= 4) {
            last = time(NULL);
            poll_state();
            draw();
        }
    }
    return 0;
}
