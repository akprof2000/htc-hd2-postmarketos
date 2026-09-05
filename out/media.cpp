// Медиаплеер HTC HD2 — нативная замена media на Python/Tk.
//
// Ищет музыку в /root/Music и на карте, декодирует ffmpeg-ом и играет
// через pcmplay (легаси-звук MSM). Список прокручивается пальцем.
//
// Сборка: g++ -O2 media.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o media

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <dirent.h>
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

#include <algorithm>
#include <string>
#include <vector>

static const int W = 480, H = 752, WIN_Y = 48;
static const unsigned long BG = 0x101828, KEY = 0x1c2a40, SEL = 0x2a3a4a,
                           ACC = 0x1f7a33, STOP = 0xa4262c;
static const int NOW_Y = 34;
static const int LIST_Y = 52, LIST_H = 620, ROW_H = 44;
static const int BAR_Y = 684, BAR_H = 76;

static const char *DIRS[] = {"/root/Music", "/mnt/fat", "/mnt/fat/Music", NULL};
static const char *EXT[] = {".mp3", ".wav", ".ogg", ".flac", ".m4a", ".aac",
                            ".wma", NULL};

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_now, *f_row, *f_btn;
static XftColor c_fg, c_dim;

static std::vector<std::string> files;   // полные пути
static int sel = -1, scroll = 0;
static pid_t proc = 0;
static int headset = 0;
static std::string now_txt = "—";

static std::string base(const std::string &p)
{
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

static void scan(void)
{
    files.clear();
    for (int i = 0; DIRS[i]; i++) {
        DIR *d = opendir(DIRS[i]);
        if (!d)
            continue;
        std::vector<std::string> here;
        struct dirent *e;
        while ((e = readdir(d))) {
            std::string n = e->d_name;
            std::string low = n;
            for (char &c : low)
                c = tolower((unsigned char)c);
            for (int k = 0; EXT[k]; k++) {
                size_t l = strlen(EXT[k]);
                if (low.size() > l && !low.compare(low.size() - l, l, EXT[k])) {
                    here.push_back(std::string(DIRS[i]) + "/" + n);
                    break;
                }
            }
        }
        closedir(d);
        std::sort(here.begin(), here.end());
        files.insert(files.end(), here.begin(), here.end());
    }
}

static int playing(void)
{
    if (proc <= 0)
        return 0;
    // группу мы создали сами, поэтому достаточно проверить лидера
    if (kill(proc, 0) < 0) {
        proc = 0;
        return 0;
    }
    return 1;
}

static void stop_play(void)
{
    if (proc > 0) {
        kill(-proc, SIGKILL);
        waitpid(proc, NULL, WNOHANG);
        proc = 0;
    }
    now_txt = "—";
}

static void start_play(void)
{
    if (sel < 0 || sel >= (int)files.size())
        return;
    stop_play();
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v quiet -i \"%s\" -f s16le -ac 2 -ar 44100 - | "
             "/usr/local/bin/pcmplay %s", files[sel].c_str(),
             headset ? "h" : "");
    pid_t p = fork();
    if (p == 0) {
        setsid();
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    proc = p;
    now_txt = "▶ " + base(files[sel]);
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

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);

    std::string n = now_txt;
    while (!n.empty() && tw(f_now, n.c_str()) > W - 24)
        n.erase(n.size() - 1);
    text(f_now, &c_fg, (W - tw(f_now, n.c_str())) / 2, NOW_Y, n.c_str());

    XSetForeground(dpy, gc, KEY);
    XFillRectangle(dpy, buf, gc, 10, LIST_Y, W - 20, LIST_H);
    if (files.empty()) {
        text(f_row, &c_dim, 24, LIST_Y + 40,
             "нет музыки — закиньте файлы");
        text(f_row, &c_dim, 24, LIST_Y + 72, "в /root/Music или на карту");
    }
    int rows = LIST_H / ROW_H;
    for (int i = 0; i < rows; i++) {
        int idx = scroll + i;
        if (idx >= (int)files.size())
            break;
        int y = LIST_Y + i * ROW_H;
        if (idx == sel) {
            XSetForeground(dpy, gc, SEL);
            XFillRectangle(dpy, buf, gc, 10, y, W - 20, ROW_H);
        }
        std::string nm = base(files[idx]);
        while (!nm.empty() && tw(f_row, nm.c_str()) > W - 48)
            nm.erase(nm.size() - 1);
        text(f_row, &c_fg, 24, y + 29, nm.c_str());
    }

    int hw = (W - 28) / 2;
    int on = playing();
    XSetForeground(dpy, gc, on ? STOP : ACC);
    XFillRectangle(dpy, buf, gc, 10, BAR_Y, hw, BAR_H);
    const char *pl = on ? "■ Стоп" : "▶ Играть";
    text(f_btn, &c_fg, 10 + (hw - tw(f_btn, pl)) / 2, BAR_Y + 48, pl);
    XSetForeground(dpy, gc, KEY);
    XFillRectangle(dpy, buf, gc, 18 + hw, BAR_Y, hw, BAR_H);
    const char *rt = headset ? "Наушники" : "Динамик";
    text(f_btn, &c_fg, 18 + hw + (hw - tw(f_btn, rt)) / 2, BAR_Y + 48, rt);

    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

int main(void)
{
    int lock = open("/run/.media.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Медиа");
    XClassHint ch;
    ch.res_name = (char *)"media";
    ch.res_class = (char *)"Media";
    XSetClassHint(dpy, win, &ch);
    XSizeHints sh;
    sh.flags = PPosition | PSize | PMinSize | PMaxSize;
    sh.x = 0; sh.y = WIN_Y;
    sh.width = sh.min_width = sh.max_width = W;
    sh.height = sh.min_height = sh.max_height = H;
    XSetWMNormalHints(dpy, win, &sh);
    Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask |
                 ButtonReleaseMask | Button1MotionMask);
    XMapWindow(dpy, win);
    buf = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, buf, 0, NULL);
    xd = XftDrawCreate(dpy, buf, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));
    f_now = XftFontOpenName(dpy, scr, "DejaVu Sans:size=13:bold");
    f_row = XftFontOpenName(dpy, scr, "DejaVu Sans:size=13");
    f_btn = XftFontOpenName(dpy, scr, "DejaVu Sans:size=15:bold");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);

    scan();
    int xfd = ConnectionNumber(dpy);
    int press_y = 0, moved = 0, scroll_at_press = 0;
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose)
                draw();
            else if (e.type == ClientMessage &&
                     (Atom)e.xclient.data.l[0] == wm_del) {
                stop_play();
                return 0;
            } else if (e.type == ButtonPress) {
                press_y = e.xbutton.y;
                moved = 0;
                scroll_at_press = scroll;
            } else if (e.type == MotionNotify) {
                // прокрутка только пальцем по экрану, а не наведением
                if (!(e.xmotion.state & Button1Mask))
                    continue;
                int d = (press_y - e.xmotion.y) / ROW_H;
                int rows = LIST_H / ROW_H;
                int maxs = (int)files.size() - rows;
                if (maxs < 0)
                    maxs = 0;
                int ns = scroll_at_press + d;
                if (ns < 0) ns = 0;
                if (ns > maxs) ns = maxs;
                if (abs(press_y - e.xmotion.y) > 12)
                    moved = 1;
                if (ns != scroll) {
                    scroll = ns;
                    draw();
                }
            } else if (e.type == ButtonRelease) {
                int x = e.xbutton.x, y = e.xbutton.y;
                if (moved)
                    continue;
                if (y >= BAR_Y && y < BAR_Y + BAR_H) {
                    int hw = (W - 28) / 2;
                    if (x < 10 + hw) {
                        if (playing())
                            stop_play();
                        else {
                            if (sel < 0 && !files.empty())
                                sel = 0;
                            start_play();
                        }
                    } else {
                        headset = !headset;
                        if (playing())
                            start_play();   // перезапуск на новом выходе
                    }
                } else if (y >= LIST_Y && y < LIST_Y + LIST_H) {
                    int idx = scroll + (y - LIST_Y) / ROW_H;
                    if (idx < (int)files.size())
                        sel = idx;
                }
                draw();
            }
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {1, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);
        if (proc > 0 && !playing()) {      // трек кончился сам
            now_txt = "—";
            draw();
        }
    }
    return 0;
}
