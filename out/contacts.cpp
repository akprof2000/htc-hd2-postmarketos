// Записная книжка HTC HD2.
//
// Хранит записи в /root/.contacts строками «имя<таб>номер». Тем же
// файлом пользуются экран вызова, звонилка и сообщения, чтобы вместо
// номера показывать имя — сравнение по последним десяти цифрам, так
// «+7 916…», «8 916…» и «916…» считаются одним человеком.
//
// Три экрана: список, карточка (позвонить, написать, удалить) и
// правка. Клавиатура поднимается только когда ткнули в поле ввода.
//
// Сборка: g++ -O2 contacts.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o contacts

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
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

static const int W = 480, H = 752, WIN_Y = 48;
static const unsigned long BG = 0x101418, KEYC = 0x1c2530, ACCENT = 0x2e7d32,
                           FIELD = 0x1c2530, RED = 0xa4262c, BLUE = 0x0a6ebd;
static const char *FILEPATH = "/root/.contacts";
static const int HEAD_H = 56, ROW_H = 76;

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_big, *f_txt, *f_small;
static XftColor c_fg, c_dim;

struct Rec { std::string name, num; };
static std::vector<Rec> recs;

enum { SCR_LIST, SCR_CARD, SCR_EDIT };
static int screen = SCR_LIST;
static int cur = -1, scroll = 0, focus = 0, kbd_up = 0;
static std::string e_name, e_num;
static std::string status;

// ── файл ─────────────────────────────────────────────────────────────
static std::string readf(const char *p)
{
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return "";
    std::string s;
    char b[4096];
    ssize_t n;
    while ((n = read(fd, b, sizeof(b))) > 0)
        s.append(b, n);
    close(fd);
    return s;
}

static bool by_name(const Rec &a, const Rec &b) { return a.name < b.name; }

static void load(void)
{
    recs.clear();
    std::string s = readf(FILEPATH);
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find('\n', p);
        if (e == std::string::npos)
            e = s.size();
        std::string ln = s.substr(p, e - p);
        p = e + 1;
        size_t t = ln.find('\t');
        if (t == std::string::npos)
            continue;
        Rec r;
        r.name = ln.substr(0, t);
        r.num = ln.substr(t + 1);
        while (!r.num.empty() && (r.num.back() == '\r' || r.num.back() == ' '))
            r.num.pop_back();
        if (!r.name.empty())
            recs.push_back(r);
    }
    std::sort(recs.begin(), recs.end(), by_name);
}

static void save(void)
{
    std::string s;
    for (size_t i = 0; i < recs.size(); i++)
        s += recs[i].name + "\t" + recs[i].num + "\n";
    int fd = open(FILEPATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    if (write(fd, s.c_str(), s.size()) < 0) { }
    close(fd);
}

// ── общее ────────────────────────────────────────────────────────────
static std::string pretty(const std::string &n)
{
    std::string d;
    for (size_t i = 0; i < n.size(); i++)
        if (isdigit((unsigned char)n[i]))
            d += n[i];
    if (d.size() < 10)
        return n;
    std::string t = d.substr(d.size() - 10);
    return "+7 " + t.substr(0, 3) + " " + t.substr(3, 3) + "-" +
           t.substr(6, 2) + "-" + t.substr(8, 2);
}

static void kbd_show(void)
{
    if (kbd_up)
        return;
    int fd = open("/run/kbd.want", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char b[16];
        int n = snprintf(b, sizeof(b), "%d\n", (int)getpid());
        if (write(fd, b, n) < 0) { }
        close(fd);
    }
    kbd_up = 1;
}

static void kbd_hide(void)
{
    unlink("/run/kbd.want");
    kbd_up = 0;
}

static void run(const char *cmd)
{
    if (fork() == 0) {
        setsid();
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 1); dup2(null, 2); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static void at_cmd(const std::string &s)
{
    int fd = open("/run/phone/cmd", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return;
    std::string l = s + "\n";
    if (write(fd, l.c_str(), l.size()) < 0) { }
    close(fd);
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

static void fill(unsigned long col, int x, int y, int w, int h)
{
    XSetForeground(dpy, gc, col);
    XFillRectangle(dpy, buf, gc, x, y, w, h);
}

static std::string cut(XftFont *fn, std::string s, int width)
{
    if (tw(fn, s.c_str()) <= width)
        return s;
    while (!s.empty() && tw(fn, (s + "…").c_str()) > width) {
        while (!s.empty() && ((unsigned char)s.back() & 0xc0) == 0x80)
            s.erase(s.size() - 1);
        if (!s.empty())
            s.erase(s.size() - 1);
    }
    return s + "…";
}

static void button(int x, int y, int w, int h, unsigned long col,
                   const char *label)
{
    fill(col, x, y, w, h);
    XftFont *fn = tw(f_big, label) <= w - 16 ? f_big : f_txt;
    text(fn, &c_fg, x + (w - tw(fn, label)) / 2, y + h / 2 + 8, label);
}

static void draw_head(const char *title, int back)
{
    fill(KEYC, 0, 0, W, HEAD_H);
    if (back) {
        text(f_txt, &c_dim, 14, 36, "‹  Назад");
        text(f_big, &c_fg, 140, 36, title);
    } else
        text(f_big, &c_fg, 14, 36, title);
}

static void draw_list(void)
{
    fill(BG, 0, 0, W, H);
    draw_head("Контакты", 0);
    int y0 = HEAD_H + 4, bottom = H - 74;
    if (recs.empty())
        text(f_txt, &c_dim, 20, y0 + 40, "пусто — нажмите «Добавить»");
    for (size_t i = (size_t)scroll; i < recs.size(); i++) {
        int y = y0 + ((int)i - scroll) * ROW_H;
        if (y + ROW_H > bottom)
            break;
        fill(KEYC, 10, y, W - 20, ROW_H - 6);
        text(f_big, &c_fg, 24, y + 30, cut(f_big, recs[i].name, W - 48).c_str());
        text(f_txt, &c_dim, 24, y + 56,
             cut(f_txt, pretty(recs[i].num), W - 48).c_str());
    }
    button(10, H - 68, W - 20, 58, ACCENT, "Добавить");
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void draw_card(void)
{
    fill(BG, 0, 0, W, H);
    const Rec &r = recs[cur];
    draw_head(cut(f_big, r.name, W - 160).c_str(), 1);
    text(f_big, &c_fg, (W - tw(f_big, pretty(r.num).c_str())) / 2, 140,
         pretty(r.num).c_str());
    button(20, 200, W - 40, 90, ACCENT, "Позвонить");
    button(20, 306, W - 40, 90, BLUE, "Написать");
    button(20, 412, W - 40, 80, KEYC, "Изменить");
    button(20, 508, W - 40, 80, RED, "Удалить");
    if (!status.empty())
        text(f_small, &c_dim, 20, 620, status.c_str());
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static int edit_y(int i) { return HEAD_H + 26 + i * 96; }

static void draw_edit(void)
{
    fill(BG, 0, 0, W, H);
    draw_head(cur >= 0 ? "Правка" : "Новая запись", 1);
    const char *lbl[2] = {"Имя", "Номер"};
    std::string *val[2] = {&e_name, &e_num};
    for (int i = 0; i < 2; i++) {
        int y = edit_y(i);
        text(f_small, &c_dim, 14, y, lbl[i]);
        fill(FIELD, 10, y + 8, W - 20, 52);
        if (focus == i) {
            XSetForeground(dpy, gc, 0x3a5578);
            XDrawRectangle(dpy, buf, gc, 10, y + 8, W - 21, 51);
        }
        text(f_big, &c_fg, 22, y + 44, cut(f_big, *val[i], W - 44).c_str());
    }
    button(10, edit_y(2), W - 20, 64,
           e_name.empty() || e_num.empty() ? KEYC : ACCENT, "Сохранить");
    text(f_small, &c_dim, 14, edit_y(2) + 100,
         "номер набирается на слое 123 клавиатуры");
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void draw(void)
{
    if (screen == SCR_LIST) draw_list();
    else if (screen == SCR_CARD) draw_card();
    else draw_edit();
}

// ── нажатия ──────────────────────────────────────────────────────────
static void click(int x, int y)
{
    (void)x;
    if (screen == SCR_LIST) {
        if (y >= H - 68) {
            screen = SCR_EDIT;
            cur = -1;
            e_name.clear();
            e_num.clear();
            focus = 0;
            kbd_show();
            return;
        }
        int i = scroll + (y - HEAD_H - 4) / ROW_H;
        if (y > HEAD_H && i >= 0 && i < (int)recs.size()) {
            cur = i;
            screen = SCR_CARD;
            status.clear();
        }
        return;
    }
    if (screen == SCR_CARD) {
        if (y < HEAD_H) {
            screen = SCR_LIST;
            return;
        }
        if (y >= 200 && y < 290) {              // позвонить
            at_cmd("ATD" + recs[cur].num + ";");
            status = "звоню…";
            return;
        }
        if (y >= 306 && y < 396) {              // написать
            run("DISPLAY=:0 /usr/local/bin/phone-sms");
            status = "открываю сообщения";
            return;
        }
        if (y >= 412 && y < 492) {              // изменить
            e_name = recs[cur].name;
            e_num = recs[cur].num;
            screen = SCR_EDIT;
            focus = 0;
            kbd_show();
            return;
        }
        if (y >= 508 && y < 588) {              // удалить
            recs.erase(recs.begin() + cur);
            save();
            cur = -1;
            screen = SCR_LIST;
        }
        return;
    }
    // правка
    if (y < HEAD_H) {
        screen = cur >= 0 ? SCR_CARD : SCR_LIST;
        kbd_hide();
        return;
    }
    for (int i = 0; i < 2; i++) {
        int fy = edit_y(i) + 8;
        if (y >= fy && y < fy + 52) {
            focus = i;
            kbd_show();
            return;
        }
    }
    int sy = edit_y(2);
    if (y >= sy && y < sy + 64 && !e_name.empty() && !e_num.empty()) {
        if (cur >= 0 && cur < (int)recs.size()) {
            recs[cur].name = e_name;
            recs[cur].num = e_num;
        } else {
            Rec r;
            r.name = e_name;
            r.num = e_num;
            recs.push_back(r);
        }
        std::sort(recs.begin(), recs.end(), by_name);
        save();
        load();
        screen = SCR_LIST;
        cur = -1;
        kbd_hide();
    }
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
    if (screen != SCR_EDIT)
        return;
    char b[16];
    KeySym ks;
    int n = XLookupString(e, b, sizeof(b) - 1, &ks, NULL);
    std::string *dst = focus == 0 ? &e_name : &e_num;
    if (ks == XK_BackSpace) {
        while (!dst->empty() && ((unsigned char)dst->back() & 0xc0) == 0x80)
            dst->erase(dst->size() - 1);
        if (!dst->empty())
            dst->erase(dst->size() - 1);
    } else if (ks == XK_Return)
        focus = (focus + 1) % 2;
    else if (n > 0) {
        b[n] = 0;
        *dst += b;
    } else if (ks >= 0x01000000)
        *dst += cp_to_utf8(ks & 0xffffff);
    draw();
}

int main(void)
{
    int lock = open("/run/.contacts.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    load();

    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Контакты");
    XClassHint ch;
    ch.res_name = (char *)"contacts";
    ch.res_class = (char *)"Contacts";
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
                 ButtonReleaseMask | Button1MotionMask | KeyPressMask);
    XMapWindow(dpy, win);
    buf = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, buf, 0, NULL);
    xd = XftDrawCreate(dpy, buf, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));
    f_big = XftFontOpenName(dpy, scr, "DejaVu Sans:size=15:bold");
    f_txt = XftFontOpenName(dpy, scr, "DejaVu Sans:size=13");
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);

    int xfd = ConnectionNumber(dpy);
    int press_y = 0, moved = 0, scroll_at_press = 0;
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose)
                draw();
            else if (e.type == ButtonPress) {
                press_y = e.xbutton.y;
                moved = 0;
                scroll_at_press = scroll;
            } else if (e.type == MotionNotify) {
                if (!(e.xmotion.state & Button1Mask))
                    continue;
                int dy = e.xmotion.y - press_y;
                if (abs(dy) > 14)
                    moved = 1;
                if (screen != SCR_LIST)
                    continue;
                int maxs = (int)recs.size() - 7;
                if (maxs < 0)
                    maxs = 0;
                int ns = scroll_at_press - dy / ROW_H;
                if (ns < 0) ns = 0;
                if (ns > maxs) ns = maxs;
                if (ns != scroll) {
                    scroll = ns;
                    draw();
                }
            } else if (e.type == ButtonRelease) {
                if (!moved) {
                    click(e.xbutton.x, e.xbutton.y);
                    draw();
                }
            } else if (e.type == KeyPress)
                key_in(&e.xkey);
            else if (e.type == ClientMessage &&
                     (Atom)e.xclient.data.l[0] == wm_del) {
                kbd_hide();
                return 0;
            }
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {1, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);
    }
    return 0;
}
