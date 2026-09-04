// Шторка уведомлений IceMobile — нативная замена прежней на Python/Tk.
//
// Резидент: держит окно готовым и показывает его по команде в
// /run/shade.fifo ("show", "hide", "vol N"). Рисуем через Xlib, текст —
// Xft (кириллица). Быстрые тумблеры + уведомления + полоска громкости.
//
// Сборка: g++ -O2 shade.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o shade

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

static const int W = 480, H = 780;
static const int VW = 300, VH = 86;    // окно громкости
static const unsigned long BG = 0x0d1420, KEYC = 0x22314a, ACC = 0x1f7a33;
static const char *FIFO = "/run/shade.fifo";
static const char *TORCH = "/sys/class/leds/flashlight/brightness";

static Display *dpy;
static int scr;
static Window win, vwin;
static Pixmap buf;
static GC gc;
static XftDraw *xd, *xdv;
static XftFont *f_big, *f_mid, *f_small;
static XftColor c_fg, c_dim;
static int shown = 0, vshown = 0;
static time_t hide_at = 0, vhide_at = 0;
static int cur_vol = 70;

// ── мелкие помощники ─────────────────────────────────────────────────
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

static void writef(const char *path, const char *val)
{
    // O_TRUNC обязателен: без него «90» поверх «100» даёт «900»
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0)
        return;
    if (write(fd, val, strlen(val)) < 0) { /* не критично */ }
    close(fd);
}

static void buzz(void) { writef("/sys/class/timed_output/vibrator/enable", "30"); }

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

// вывод команды в строку (для iw / hciconfig / crontab)
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
    return out;
}

static void at_cmd(const char *c)
{
    int fd = open("/run/phone/cmd", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return;
    std::string s = std::string(c) + "\n";
    if (write(fd, s.c_str(), s.size()) < 0) { /* ничего */ }
    close(fd);
}

static int wifi_on(void) { return capture("iw dev wlan0 link 2>/dev/null").find("Connected") != std::string::npos; }
static int bt_on(void) { return capture("hciconfig hci0 2>/dev/null").find("UP RUNNING") != std::string::npos; }
static int torch_on(void) { std::string v = readf(TORCH); return !v.empty() && v != "0"; }

// ── тумблеры ─────────────────────────────────────────────────────────
struct Toggle {
    const char *name;
    int (*state)(void);                // подсветка, если включено
    void (*act)(void);
};

static int st_wifi(void) { return wifi_on(); }
static int st_torch(void) { return torch_on(); }
static int st_sound(void) { return readf("/run/phone/vol") != "0"; }
static int st_bt(void) { return bt_on(); }
static int st_2g(void) { std::string n = readf("/run/phone/net"); return n.compare(0, 4, "EDGE") == 0 || n.compare(0, 3, "GSM") == 0; }
static int st_3g(void) { return readf("/run/phone/net").compare(0, 2, "3G") == 0; }
static int st_no(void) { return 0; }

static void a_wifi(void) { sh(wifi_on() ? "ifconfig wlan0 down" : "ifconfig wlan0 up"); }
static void a_torch(void) { sh("/usr/local/bin/torch"); }
static void a_sound(void)
{
    int off = readf("/run/phone/vol") == "0";
    at_cmd(off ? "@vol 70" : "@vol 0");
    writef("/run/phone/vol", off ? "70" : "0");
}
static void a_bt(void)
{
    if (bt_on())
        sh("hciconfig hci0 down; echo 0 > /sys/class/rfkill/rfkill0/state");
    else
        sh("/usr/local/bin/btsetup");
}
static void a_2g(void) { at_cmd("AT+COPS=1,2,\"25002\",0"); }
static void a_3g(void) { at_cmd("AT+COPS=0"); }
static void a_bright(void) { sh("/usr/local/bin/backlight +"); }
static void a_dim(void) { sh("/usr/local/bin/backlight -"); }

static const Toggle TOGGLES[] = {
    {"Wi-Fi",  st_wifi,  a_wifi},
    {"Фонарь", st_torch, a_torch},
    {"Звук",   st_sound, a_sound},
    {"BT",     st_bt,    a_bt},
    {"2G",     st_2g,    a_2g},
    {"3G",     st_3g,    a_3g},
    {"Ярче",   st_no,    a_bright},
    {"Темнее", st_no,    a_dim},
};
static const int NTOG = sizeof(TOGGLES) / sizeof(TOGGLES[0]);

// ── уведомления ──────────────────────────────────────────────────────
struct Note {
    std::string text;
    std::string cmd;                   // что открыть по тапу
};
static std::vector<Note> notes;

static void collect_notes(void)
{
    notes.clear();
    std::string missed = readf("/run/phone/missed");
    if (!missed.empty() && missed != "0")
        notes.push_back({"Пропущенные вызовы: " + missed,
                         "DISPLAY=:0 /usr/local/bin/phone-gui"});
    std::string sms = readf("/run/phone/sms_new");
    int cnt = 0;
    for (char ch : sms)
        if (ch == '\n')
            cnt++;
    if (!sms.empty() && cnt == 0)
        cnt = 1;
    if (cnt) {
        char b[64];
        snprintf(b, sizeof(b), "Новых SMS: %d", cnt);
        notes.push_back({b, "DISPLAY=:0 /usr/local/bin/phone-sms"});
    }
    std::string cron = capture("crontab -l 2>/dev/null");
    size_t at = cron.find("icemobile-alarm");
    if (at != std::string::npos) {
        int mi = 0, hh = 0;
        size_t ls = cron.rfind('\n', at);
        ls = (ls == std::string::npos) ? 0 : ls + 1;
        if (sscanf(cron.c_str() + ls, "%d %d", &mi, &hh) == 2) {
            char b[64];
            snprintf(b, sizeof(b), "Будильник на %02d:%02d", hh, mi);
            notes.push_back({b, "DISPLAY=:0 /usr/local/bin/clock-app"});
        }
    }
    std::string cap = readf("/sys/class/power_supply/battery/capacity");
    if (!cap.empty() && atoi(cap.c_str()) <= 20)
        notes.push_back({"Заряд батареи низкий: " + cap + "%", ""});
    if (notes.empty())
        notes.push_back({"Нет новых уведомлений", ""});
}

// ── отрисовка ────────────────────────────────────────────────────────
static void text(XftDraw *d, XftFont *fn, XftColor *col, int x, int y,
                 const char *s)
{
    XftDrawStringUtf8(d, col, fn, x, y, (const FcChar8 *)s, strlen(s));
}

static int text_w(XftFont *fn, const char *s)
{
    XGlyphInfo gi;
    XftTextExtentsUtf8(dpy, fn, (const FcChar8 *)s, strlen(s), &gi);
    return gi.xOff;
}

static const int TOG_Y = 92, TOG_H = 74, TOG_COLS = 4;
// NOTE_Y-10 — базовая линия подписи «Уведомления»: буквы уходят вверх,
// и при 268 она ложилась на нижний ряд тумблеров (тот кончается на 246)
static const int NOTE_Y = 292, NOTE_H = 56;
static const int CLOSE_Y = H - 54;

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);

    // часы, дата, заряд
    char hm[16], dm[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(hm, sizeof(hm), "%H:%M", &tm);
    static const char *WD[] = {"вс", "пн", "вт", "ср", "чт", "пт", "сб"};
    char dd[16];
    strftime(dd, sizeof(dd), "%d.%m", &tm);
    snprintf(dm, sizeof(dm), "%s, %s", WD[tm.tm_wday], dd);
    text(xd, f_big, &c_fg, 14, 48, hm);
    text(xd, f_mid, &c_dim, 20 + text_w(f_big, hm), 48, dm);
    std::string cap = readf("/sys/class/power_supply/battery/capacity");
    std::string bat = cap + "%";
    text(xd, f_mid, &c_dim, W - 16 - text_w(f_mid, bat.c_str()), 48,
         bat.c_str());

    // тумблеры
    int tw = (W - 20 - (TOG_COLS - 1) * 6) / TOG_COLS;
    for (int i = 0; i < NTOG; i++) {
        int c = i % TOG_COLS, r = i / TOG_COLS;
        int x = 10 + c * (tw + 6), y = TOG_Y + r * (TOG_H + 6);
        XSetForeground(dpy, gc, TOGGLES[i].state() ? ACC : KEYC);
        XFillRectangle(dpy, buf, gc, x, y, tw, TOG_H);
        // длинные названия не влезают крупным шрифтом
        const char *nm = TOGGLES[i].name;
        XftFont *fn = text_w(f_mid, nm) > tw - 8 ? f_small : f_mid;
        text(xd, fn, &c_fg, x + (tw - text_w(fn, nm)) / 2,
             y + TOG_H / 2 + 6, nm);
    }

    // уведомления
    text(xd, f_small, &c_dim, 14, NOTE_Y - 10, "Уведомления");
    for (size_t i = 0; i < notes.size() && i < 6; i++) {
        int y = NOTE_Y + (int)i * (NOTE_H + 6);
        XSetForeground(dpy, gc, 0x182238);
        XFillRectangle(dpy, buf, gc, 10, y, W - 20, NOTE_H);
        text(xd, f_mid, &c_fg, 24, y + 34, notes[i].text.c_str());
    }

    // полоса закрытия
    XSetForeground(dpy, gc, KEYC);
    XFillRectangle(dpy, buf, gc, 0, CLOSE_Y, W, H - CLOSE_Y);
    const char *cl = "закрыть";
    text(xd, f_mid, &c_dim, (W - text_w(f_mid, cl)) / 2, CLOSE_Y + 34, cl);

    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void draw_vol(void)
{
    XSetForeground(dpy, gc, KEYC);
    XFillRectangle(dpy, vwin, gc, 0, 0, VW, VH);
    char t[48];
    snprintf(t, sizeof(t), "Громкость %d%%", cur_vol);
    text(xdv, f_mid, &c_fg, (VW - text_w(f_mid, t)) / 2, 34, t);
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, vwin, gc, 20, 50, VW - 40, 18);
    XSetForeground(dpy, gc, ACC);
    XFillRectangle(dpy, vwin, gc, 20, 50,
                   (VW - 40) * (cur_vol < 0 ? 0 : cur_vol > 100 ? 100
                                : cur_vol) / 100, 18);
    XFlush(dpy);
}

static void show(void)
{
    collect_notes();
    XMapRaised(dpy, win);
    shown = 1;
    hide_at = time(NULL) + 20;         // сама уедет через 20 с бездействия
    draw();
}

static void hide(void)
{
    XUnmapWindow(dpy, win);
    shown = 0;
    XFlush(dpy);
}

static void show_vol(int v)
{
    cur_vol = v;
    XMapRaised(dpy, vwin);
    vshown = 1;
    vhide_at = time(NULL) + 2;
    draw_vol();
}

// ── нажатия ──────────────────────────────────────────────────────────
static void click(int x, int y)
{
    if (y >= CLOSE_Y) {
        hide();
        return;
    }
    int tw = (W - 20 - (TOG_COLS - 1) * 6) / TOG_COLS;
    for (int i = 0; i < NTOG; i++) {
        int c = i % TOG_COLS, r = i / TOG_COLS;
        int tx = 10 + c * (tw + 6), ty = TOG_Y + r * (TOG_H + 6);
        if (x >= tx && x < tx + tw && y >= ty && y < ty + TOG_H) {
            buzz();
            TOGGLES[i].act();
            hide_at = time(NULL) + 20;
            draw();                    // подсветку обновим сразу
            return;
        }
    }
    for (size_t i = 0; i < notes.size() && i < 6; i++) {
        int ny = NOTE_Y + (int)i * (NOTE_H + 6);
        if (y >= ny && y < ny + NOTE_H && !notes[i].cmd.empty()) {
            sh(notes[i].cmd.c_str());
            hide();
            return;
        }
    }
}

int main(int argc, char **argv)
{
    int daemon_mode = (argc > 1 && strcmp(argv[1], "--daemon") == 0);
    if (!daemon_mode) {
        // клиент: просто просим резидента показаться
        int fd = open(FIFO, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            if (write(fd, "show\n", 5) < 0) { }
            close(fd);
            return 0;
        }
        // резидента нет — поднимаем его и повторяем
        sh("setsid /usr/local/bin/shade --daemon >/dev/null 2>&1");
        for (int i = 0; i < 40; i++) {
            usleep(300000);
            fd = open(FIFO, O_WRONLY | O_NONBLOCK);
            if (fd >= 0) {
                if (write(fd, "show\n", 5) < 0) { }
                close(fd);
                return 0;
            }
        }
        return 1;
    }

    // O_CLOEXEC обязателен: без него запущенные нами программы
    // наследуют эту блокировку и держат её после нашего выхода —
    // тогда следующий экземпляр уже не стартует (так шторка
    // держала блокировку статус-полоски)
    int lock = open("/tmp/.shaded.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);

    mkfifo(FIFO, 0666);
    int ffd = open(FIFO, O_RDWR | O_NONBLOCK);
    if (ffd < 0) {
        fprintf(stderr, "нет канала %s\n", FIFO);
        return 1;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "нет доступа к X\n");
        return 1;
    }
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Шторка");
    XClassHint ch;
    ch.res_name = (char *)"shade";
    ch.res_class = (char *)"Shade";
    XSetClassHint(dpy, win, &ch);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask);

    vwin = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 90, 330, VW, VH,
                               0, KEYC, KEYC);
    XStoreName(dpy, vwin, "volosd");
    XClassHint vh;
    vh.res_name = (char *)"volosd";
    vh.res_class = (char *)"Volosd";
    XSetClassHint(dpy, vwin, &vh);
    XSelectInput(dpy, vwin, ExposureMask);

    buf = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, buf, 0, NULL);
    xd = XftDrawCreate(dpy, buf, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));
    xdv = XftDrawCreate(dpy, vwin, DefaultVisual(dpy, scr),
                        DefaultColormap(dpy, scr));
    f_big = XftFontOpenName(dpy, scr, "DejaVu Sans:size=26:bold");
    f_mid = XftFontOpenName(dpy, scr, "DejaVu Sans:size=13:bold");
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11");
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr), &wc, &c_fg);
    XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr), &dc, &c_dim);

    int xfd = ConnectionNumber(dpy);
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose) {
                if (e.xexpose.window == win && shown)
                    draw();
                else if (e.xexpose.window == vwin && vshown)
                    draw_vol();
            } else if (e.type == ButtonPress && e.xbutton.window == win) {
                click(e.xbutton.x, e.xbutton.y);
            }
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        FD_SET(ffd, &fds);
        int mx = xfd > ffd ? xfd : ffd;
        struct timeval tv = {0, 300000};
        select(mx + 1, &fds, NULL, NULL, &tv);

        if (FD_ISSET(ffd, &fds)) {
            char b[128];
            ssize_t n = read(ffd, b, sizeof(b) - 1);
            if (n > 0) {
                b[n] = 0;
                if (!strncmp(b, "vol", 3))
                    show_vol(atoi(b + 3));
                else if (strstr(b, "show"))
                    show();
                else if (strstr(b, "hide"))
                    hide();
            }
        }
        time_t now = time(NULL);
        if (shown && now > hide_at)
            hide();
        if (vshown && now > vhide_at) {
            XUnmapWindow(dpy, vwin);
            vshown = 0;
            XFlush(dpy);
        }
    }
    return 0;
}
