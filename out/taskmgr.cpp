// Менеджер приложений HTC HD2 — нативная замена taskmgr на Python/Tk.
//
// Список окон берём у wmctrl -lp, закрываем вежливо (wmctrl -ic), а через
// две секунды добиваем SIGKILL, если процесс не ушёл сам.
//
// Сборка: g++ -O2 taskmgr.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o taskmgr

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
static const unsigned long BG = 0x101828, ROW = 0x1c2a40, KILL = 0xa4262c,
                           BTN = 0x33415c;
static const int LIST_Y = 96, ROW_H = 58, GAP = 6, FOOT_H = 64, KILL_W = 150;

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_head, *f_small, *f_row;
static XftColor c_fg, c_dim;

struct App { unsigned long wid; int pid; std::string title; };
static std::vector<App> apps;
static std::string memline;

// эти не выгружаем — без них телефон останется без интерфейса
static int keep(const std::string &t)
{
    return t == "Домой" || t == "Приложения" || t == "Меню" ||
           t == "Питание" || t == "statusbar";
}

static void scan(void)
{
    apps.clear();
    FILE *f = popen("wmctrl -lp 2>/dev/null", "r");
    if (f) {
        char ln[512];
        while (fgets(ln, sizeof(ln), f)) {
            unsigned long wid = 0;
            int desk = 0, pid = 0;
            char host[128], title[256];
            // формат: 0xID  рабочий стол  pid  машина  заголовок
            if (sscanf(ln, "%lx %d %d %127s %255[^\n]", &wid, &desk, &pid,
                       host, title) != 5)
                continue;
            std::string t(title);
            while (!t.empty() && (t.back() == '\n' || t.back() == ' '))
                t.pop_back();
            if (!keep(t))
                apps.push_back({wid, pid, t});
        }
        pclose(f);
    }

    long total = 0, fr = 0, bufs = 0, cache = 0;
    FILE *m = fopen("/proc/meminfo", "r");
    if (m) {
        char ln[128];
        while (fgets(ln, sizeof(ln), m)) {
            long v = 0;
            if (sscanf(ln, "MemTotal: %ld", &v) == 1) total = v;
            else if (sscanf(ln, "MemFree: %ld", &v) == 1) fr = v;
            else if (sscanf(ln, "Buffers: %ld", &v) == 1) bufs = v;
            else if (sscanf(ln, "Cached: %ld", &v) == 1) cache = v;
        }
        fclose(m);
    }
    char b[128];
    snprintf(b, sizeof(b), "память: свободно %ld МБ из %ld",
             (fr + bufs + cache) / 1024, total / 1024);
    memline = b;
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

static int rows_fit(void) { return (H - LIST_Y - FOOT_H) / (ROW_H + GAP); }

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);
    const char *t = "Приложения";
    text(f_head, &c_fg, (W - tw(f_head, t)) / 2, 46, t);
    text(f_small, &c_dim, (W - tw(f_small, memline.c_str())) / 2, 74,
         memline.c_str());

    int n = (int)apps.size(), fit = rows_fit();
    if (n == 0) {
        const char *e = "всё выгружено";
        text(f_row, &c_dim, (W - tw(f_row, e)) / 2, LIST_Y + 40, e);
    }
    for (int i = 0; i < n && i < fit; i++) {
        int y = LIST_Y + i * (ROW_H + GAP);
        XSetForeground(dpy, gc, ROW);
        XFillRectangle(dpy, buf, gc, 12, y, W - 24 - KILL_W, ROW_H);
        XSetForeground(dpy, gc, KILL);
        XFillRectangle(dpy, buf, gc, W - 12 - KILL_W, y, KILL_W, ROW_H);
        // длинный заголовок обрезаем, чтобы не заезжал на «выгрузить»
        std::string tt = apps[i].title;
        while (!tt.empty() && tw(f_row, tt.c_str()) > W - 48 - KILL_W)
            tt.erase(tt.size() - 1);
        text(f_row, &c_fg, 24, y + ROW_H / 2 + 7, tt.c_str());
        const char *k = "✕ выгрузить";
        text(f_small, &c_fg, W - 12 - KILL_W + (KILL_W - tw(f_small, k)) / 2,
             y + ROW_H / 2 + 6, k);
    }

    int fy = H - FOOT_H + 4;
    XSetForeground(dpy, gc, BTN);
    XFillRectangle(dpy, buf, gc, 12, fy, W - 24, FOOT_H - 16);
    const char *r = "Обновить";
    text(f_row, &c_fg, (W - tw(f_row, r)) / 2, fy + 32, r);
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

// отложенный добив: ждём и, если процесс жив, снимаем его
static void force_kill(int pid)
{
    if (pid <= 1)
        return;
    if (fork() == 0) {
        setsid();
        sleep(2);
        kill(pid, SIGKILL);
        _exit(0);
    }
}

int main(void)
{
    int lock = open("/tmp/.taskmgr.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Приложения");
    XClassHint ch;
    ch.res_name = (char *)"taskmgr";
    ch.res_class = (char *)"Taskmgr";
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
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11");
    f_row = XftFontOpenName(dpy, scr, "DejaVu Sans:size=14");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);

    scan();
    int xfd = ConnectionNumber(dpy);
    time_t refresh_at = 0;
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
                int x = e.xbutton.x, y = e.xbutton.y;
                if (y >= H - FOOT_H) {
                    scan();
                    draw();
                    continue;
                }
                int i = (y - LIST_Y) / (ROW_H + GAP);
                if (y < LIST_Y || i < 0 || i >= (int)apps.size() ||
                    i >= rows_fit())
                    continue;
                int ry = LIST_Y + i * (ROW_H + GAP);
                if (y >= ry + ROW_H || x < W - 12 - KILL_W)
                    continue;           // нажали мимо кнопки «выгрузить»
                char cmd[96];
                snprintf(cmd, sizeof(cmd), "wmctrl -i -c 0x%lx", apps[i].wid);
                if (fork() == 0) {
                    setsid();
                    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
                    _exit(1);
                }
                force_kill(apps[i].pid);
                refresh_at = time(NULL) + 2;
            }
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {1, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);
        if (refresh_at && time(NULL) >= refresh_at) {
            refresh_at = 0;
            scan();
            draw();
        }
    }
    return 0;
}
