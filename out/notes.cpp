// Заметки HTC HD2 — нативная замена notes на Python/Tk.
//
// Один блокнот, сохраняется сам раз в три секунды и при закрытии. Ввод —
// экранной клавиатурой: она печатает в наше окно через XTEST, поэтому
// достаточно обычной обработки нажатий клавиш.
//
// Сборка: g++ -O2 notes.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o notes

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>

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

static const int W = 480, H = 464, WIN_Y = 48;   // снизу место под клавиатуру
static const unsigned long BG = 0x101828, FIELD = 0x1c2a40;
static const char *FILEPATH = "/root/notes.txt";

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_txt, *f_small;
static XftColor c_fg, c_dim;
static std::string body;
static int dirty = 0;

static void load(void)
{
    int fd = open(FILEPATH, O_RDONLY);
    if (fd < 0)
        return;
    char b[8192];
    ssize_t n;
    while ((n = read(fd, b, sizeof(b))) > 0)
        body.append(b, n);
    close(fd);
}

static void save(void)
{
    if (!dirty)
        return;
    int fd = open(FILEPATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    if (write(fd, body.c_str(), body.size()) < 0) { }
    close(fd);
    dirty = 0;
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

// перенос по ширине окна с учётом UTF-8 и переводов строки
static std::vector<std::string> wrap(const std::string &s, int width)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\n') {
            out.push_back(cur);
            cur.clear();
            i++;
            continue;
        }
        int n = 1;
        unsigned char c = s[i];
        if ((c & 0xe0) == 0xc0) n = 2;
        else if ((c & 0xf0) == 0xe0) n = 3;
        else if ((c & 0xf8) == 0xf0) n = 4;
        std::string ch = s.substr(i, n);
        i += n;
        if (tw(f_txt, (cur + ch).c_str()) > width) {
            out.push_back(cur);
            cur = ch;
        } else
            cur += ch;
    }
    out.push_back(cur);
    return out;
}

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);
    XSetForeground(dpy, gc, FIELD);
    XFillRectangle(dpy, buf, gc, 6, 6, W - 12, H - 34);

    std::vector<std::string> lines = wrap(body, W - 32);
    int total = (H - 60) / 24;
    size_t first = lines.size() > (size_t)total ? lines.size() - total : 0;
    int y = 30;
    for (size_t i = first; i < lines.size(); i++) {
        text(f_txt, &c_fg, 16, y, lines[i].c_str());
        y += 24;
    }
    // Курсор в конце последней строки. После цикла y уже на строку ниже
    // последней — отсюда и минус целая строка, иначе курсор стоял под
    // текстом, а не рядом с ним.
    int cx = 16 + tw(f_txt, lines.back().c_str());
    XSetForeground(dpy, gc, 0xe8eef5);
    XFillRectangle(dpy, buf, gc, cx + 2, y - 42, 2, 18);

    const char *s = "сохраняется автоматически";
    text(f_small, &c_dim, (W - tw(f_small, s)) / 2, H - 12, s);
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static std::string cp_to_utf8(unsigned cp)
{
    std::string r;
    if (cp < 0x80) r += (char)cp;
    else if (cp < 0x800) {
        r += (char)(0xc0 | (cp >> 6));
        r += (char)(0x80 | (cp & 0x3f));
    } else {
        r += (char)(0xe0 | (cp >> 12));
        r += (char)(0x80 | ((cp >> 6) & 0x3f));
        r += (char)(0x80 | (cp & 0x3f));
    }
    return r;
}

static void key_in(XKeyEvent *e)
{
    char b[16];
    KeySym ks;
    int n = XLookupString(e, b, sizeof(b) - 1, &ks, NULL);
    if (ks == XK_BackSpace) {
        while (!body.empty() && ((unsigned char)body.back() & 0xc0) == 0x80)
            body.erase(body.size() - 1);
        if (!body.empty())
            body.erase(body.size() - 1);
    } else if (ks == XK_Return)
        body += "\n";
    else if (n > 0) {
        b[n] = 0;
        body += b;
    } else if (ks >= 0x01000000)       // юникод от экранной клавиатуры
        body += cp_to_utf8(ks & 0xffffff);
    else
        return;
    dirty = 1;
    draw();
}

int main(void)
{
    int lock = open("/run/.notes.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Заметки");
    XClassHint ch;
    ch.res_name = (char *)"notes";     // по классу keysd поднимает клавиатуру
    ch.res_class = (char *)"Notes";
    XSetClassHint(dpy, win, &ch);
    XSizeHints sh;
    sh.flags = PPosition | PSize | PMinSize | PMaxSize;
    sh.x = 0; sh.y = WIN_Y;
    sh.width = sh.min_width = sh.max_width = W;
    sh.height = sh.min_height = sh.max_height = H;
    XSetWMNormalHints(dpy, win, &sh);
    Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask);
    XMapWindow(dpy, win);
    buf = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, buf, 0, NULL);
    xd = XftDrawCreate(dpy, buf, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));
    f_txt = XftFontOpenName(dpy, scr, "DejaVu Sans:size=14");
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=10");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);

    load();
    int xfd = ConnectionNumber(dpy);
    time_t last_save = time(NULL);
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose)
                draw();
            else if (e.type == KeyPress)
                key_in(&e.xkey);
            else if (e.type == ClientMessage &&
                     (Atom)e.xclient.data.l[0] == wm_del) {
                save();
                return 0;
            }
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {1, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);
        if (time(NULL) - last_save >= 3) {
            last_save = time(NULL);
            save();
        }
    }
    return 0;
}
