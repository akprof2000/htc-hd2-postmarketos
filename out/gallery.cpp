// Галерея HTC HD2 — нативная замена gallery на Python/Tk + PIL.
//
// Картинки распаковывает ffmpeg (PIL в C++ не нужен): кадр приводится к
// ровному прямоугольнику 480×700 с чёрными полями, дальше XPutImage.
// Тап слева/справа — предыдущее/следующее, по центру — пересканировать.
//
// Сборка: g++ -O2 gallery.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o gallery

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

static const int W = 480, H = 776, WIN_Y = 24;
static const int IMG_W = 480, IMG_H = 700, IMG_Y = 10;
static const unsigned long BG = 0x000000;

static const char *DIRS[] = {"/root/Pictures", "/mnt/fat", "/mnt/fat/DCIM",
                             "/mnt/fat/Pictures", NULL};
static const char *EXT[] = {".jpg", ".jpeg", ".png", ".gif", ".bmp", NULL};

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_info;
static XftColor c_dim, c_err;

static std::vector<std::string> files;
static int idx = 0;
static std::string err;
static unsigned char *pix;               // IMG_W*IMG_H*4, BGRA
static XImage *img;

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
            std::string n = e->d_name, low = n;
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

// распаковать текущий снимок в pix; пусто и err — если не вышло
static void load(void)
{
    err.clear();
    memset(pix, 0, (size_t)IMG_W * IMG_H * 4);
    if (files.empty())
        return;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v quiet -i \"%s\" -vf "
             "\"scale=%d:%d:force_original_aspect_ratio=decrease,"
             "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black\" "
             "-pix_fmt bgra -f rawvideo -frames:v 1 - 2>/dev/null",
             files[idx].c_str(), IMG_W, IMG_H, IMG_W, IMG_H);
    FILE *f = popen(cmd, "r");
    if (!f) {
        err = "не запустился ffmpeg";
        return;
    }
    size_t need = (size_t)IMG_W * IMG_H * 4, got = 0, n;
    while (got < need && (n = fread(pix + got, 1, need - got, f)) > 0)
        got += n;
    pclose(f);
    if (got < need)
        err = "не открылось: " + base(files[idx]);
}

static void text(XftColor *c, int x, int y, const char *s)
{
    XftDrawStringUtf8(xd, c, f_info, x, y, (const FcChar8 *)s, strlen(s));
}

static int tw(const char *s)
{
    XGlyphInfo gi;
    XftTextExtentsUtf8(dpy, f_info, (const FcChar8 *)s, strlen(s), &gi);
    return gi.xOff;
}

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);
    if (files.empty()) {
        const char *l1 = "нет фото";
        const char *l2 = "закиньте картинки в /root/Pictures";
        const char *l3 = "или на карту";
        text(&c_dim, (W - tw(l1)) / 2, 330, l1);
        text(&c_dim, (W - tw(l2)) / 2, 370, l2);
        text(&c_dim, (W - tw(l3)) / 2, 396, l3);
    } else if (!err.empty()) {
        text(&c_err, (W - tw(err.c_str())) / 2, 350, err.c_str());
    } else {
        XPutImage(dpy, buf, gc, img, 0, 0, 0, IMG_Y, IMG_W, IMG_H);
    }
    if (!files.empty()) {
        char b[256];
        snprintf(b, sizeof(b), "%d / %d · %s", idx + 1, (int)files.size(),
                 base(files[idx]).c_str());
        std::string s = b;
        while (!s.empty() && tw(s.c_str()) > W - 24)
            s.erase(s.size() - 1);
        text(&c_dim, (W - tw(s.c_str())) / 2, 740, s.c_str());
    }
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

int main(void)
{
    int lock = open("/tmp/.gallery.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Галерея");
    XClassHint ch;
    ch.res_name = (char *)"gallery";
    ch.res_class = (char *)"Gallery";
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
    f_info = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11");
    XRenderColor dc = {0x9a9a, 0xa4a4, 0xb0b0, 0xffff};
    XRenderColor ec = {0xefef, 0x5353, 0x5050, 0xffff};
    XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr), &dc, &c_dim);
    XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr), &ec, &c_err);

    pix = (unsigned char *)calloc((size_t)IMG_W * IMG_H, 4);
    if (!pix)
        return 1;
    img = XCreateImage(dpy, DefaultVisual(dpy, scr), 24, ZPixmap, 0,
                       (char *)pix, IMG_W, IMG_H, 32, IMG_W * 4);
    scan();
    load();
    for (;;) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == Expose)
            draw();
        else if (e.type == ClientMessage &&
                 (Atom)e.xclient.data.l[0] == wm_del)
            return 0;
        else if (e.type == ButtonPress) {
            if (files.empty())
                scan();
            else if (e.xbutton.x < 160)
                idx = (idx - 1 + (int)files.size()) % (int)files.size();
            else if (e.xbutton.x > 320)
                idx = (idx + 1) % (int)files.size();
            else {
                scan();                     // центр — пересканировать
                if (!files.empty())
                    idx %= (int)files.size();
            }
            load();
            draw();
        }
    }
    return 0;
}
