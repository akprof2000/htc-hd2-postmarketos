// Экран вызова HTC HD2 — всплывает сам при входящем.
//
// Запускает его демон phoned, как только модем сообщил о звонке. Окно
// следит за /run/phone/state и живёт ровно столько, сколько идёт вызов:
//   ringing — «Входящий вызов», кнопки «Ответить» и «Отклонить»
//   active  — номер, счётчик разговора, динамик/микрофон и «Завершить»
//   idle    — закрывается само
//
// Модему ничего не пишет напрямую: команды уходят демону в очередь
// /run/phone/cmd, он единственный владелец канала.
//
// Сборка: g++ -O2 callscreen.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o callscreen

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

static const int W = 480, H = 800;         // во весь экран, поверх полоски
static const unsigned long BG = 0x0d1520, KEYC = 0x1c2a40,
                           GREEN = 0x1f7a33, RED = 0xa4262c;
static const int NAME_Y = 210, SUB_Y = 260, TIMER_Y = 330;
static const int BTN_Y = 560, BTN_H = 150, TOOL_Y = 430, TOOL_H = 96;

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_head, *f_num, *f_btn, *f_small, *f_tiny;
static XftColor c_fg, c_dim;

static std::string state = "ringing", number, route, mute;
static time_t active_since = 0;
// Что делаем прямо сейчас: без этого окно на нажатие никак не отзывалось,
// и человек жал «Отклонить» по шесть раз подряд (видно в журнале демона).
static std::string busy_msg;
static double busy_until = 0;
static double reject_at = 0;            // когда отправили отбой

static std::string readf(const char *p)
{
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return "";
    char b[256];
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

static void at_cmd(const char *cmd)
{
    int fd = open("/run/phone/cmd", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return;
    std::string s = std::string(cmd) + "\n";
    if (write(fd, s.c_str(), s.size()) < 0) { }
    close(fd);
}

// Имя из записной книжки по номеру. Сравниваем последние десять цифр,
// чтобы «+7 916…», «8 916…» и «916…» считались одним человеком.
static std::string contact_name(const std::string &num)
{
    std::string d;
    for (size_t i = 0; i < num.size(); i++)
        if (isdigit((unsigned char)num[i]))
            d += num[i];
    if (d.size() > 10)
        d = d.substr(d.size() - 10);
    if (d.empty())
        return "";
    std::string all = readf("/root/.contacts");
    size_t p = 0;
    while (p < all.size()) {
        size_t e = all.find('\n', p);
        if (e == std::string::npos)
            e = all.size();
        std::string ln = all.substr(p, e - p);
        p = e + 1;
        size_t t = ln.find('\t');
        if (t == std::string::npos)
            continue;
        std::string cd;
        for (size_t i = t + 1; i < ln.size(); i++)
            if (isdigit((unsigned char)ln[i]))
                cd += ln[i];
        if (cd.size() > 10)
            cd = cd.substr(cd.size() - 10);
        if (cd == d)
            return ln.substr(0, t);
    }
    return "";
}

// номер в читаемом виде: последние десять цифр с разделителями
static std::string pretty(const std::string &n)
{
    std::string d;
    for (size_t i = 0; i < n.size(); i++)
        if (isdigit((unsigned char)n[i]))
            d += n[i];
    if (d.size() < 10)
        return n.empty() ? "неизвестный номер" : n;
    std::string t = d.substr(d.size() - 10);
    return "+7 " + t.substr(0, 3) + " " + t.substr(3, 3) + "-" +
           t.substr(6, 2) + "-" + t.substr(8, 2);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
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

// Подпись НЕ обрезаем, а уменьшаем шрифт до подходящего: номер длиннее
// экрана раньше терял последние цифры, а надписи на кнопках вылезали за
// их края.
static XftFont *fits(XftFont *big, XftFont *mid, XftFont *small, int width,
                     const char *s)
{
    if (tw(big, s) <= width)
        return big;
    if (tw(mid, s) <= width)
        return mid;
    return small;
}

static void centered(XftFont *fn, XftColor *c, int y, const std::string &s)
{
    XftFont *use = fits(fn, f_btn, f_small, W - 24, s.c_str());
    text(use, c, (W - tw(use, s.c_str())) / 2, y, s.c_str());
}

static void button(int x, int y, int w, int h, unsigned long col,
                   XftFont *fn, const char *label)
{
    XSetForeground(dpy, gc, col);
    XFillRectangle(dpy, buf, gc, x, y, w, h);
    XftFont *use = fits(fn, f_small, f_tiny, w - 16, label);
    text(use, &c_fg, x + (w - tw(use, label)) / 2, y + h / 2 + 8, label);
}

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);

    int ringing = (state == "ringing");
    centered(f_head, &c_dim, 120,
             ringing ? "Входящий вызов" : "Разговор");
    std::string who = contact_name(number);
    if (!who.empty()) {                    // знакомый — показываем имя
        centered(f_num, &c_fg, NAME_Y, who);
        centered(f_small, &c_dim, SUB_Y, pretty(number));
    } else {
        centered(f_num, &c_fg, NAME_Y, pretty(number));
        if (!number.empty() && pretty(number) != number)
            centered(f_small, &c_dim, SUB_Y, number);
    }

    if (!busy_msg.empty())
        centered(f_small, &c_dim, SUB_Y + 46, busy_msg);

    if (!ringing) {
        long sec = active_since ? (long)(time(NULL) - active_since) : 0;
        char t[32];
        snprintf(t, sizeof(t), "%ld:%02ld", sec / 60, sec % 60);
        centered(f_num, &c_fg, TIMER_Y, t);

        int hw = (W - 30) / 2;
        button(10, TOOL_Y, hw, TOOL_H,
               route == "speaker" ? GREEN : KEYC, f_small,
               route == "headset" ? "Наушники" : "Динамик");
        button(20 + hw, TOOL_Y, hw, TOOL_H,
               mute == "1" ? RED : KEYC, f_small,
               mute == "1" ? "Микрофон выкл" : "Микрофон вкл");
        button(10, BTN_Y, W - 20, BTN_H, RED, f_btn, "Завершить");
    } else {
        int hw = (W - 30) / 2;
        button(10, BTN_Y, hw, BTN_H, GREEN, f_btn, "Ответить");
        button(20 + hw, BTN_Y, hw, BTN_H, RED, f_btn, "Отклонить");
    }
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void click(int x, int y)
{
    if (now_s() < busy_until)          // команда уже ушла, не частим
        return;
    int hw = (W - 30) / 2;
    if (state == "ringing") {
        if (y >= BTN_Y && y < BTN_Y + BTN_H) {
            if (x < 10 + hw) {
                at_cmd("ATA");
                busy_msg = "отвечаю…";
            } else {
                // Именно AT+CHUP: от голого ATH сеть объявляет звонящему
                // «абонент недоступен» вместо «занято». ATH оставлен
                // подстраховкой, если вызов не сбросился.
                at_cmd("AT+CHUP");
                busy_msg = "отклоняю…";
                reject_at = now_s();
            }
            busy_until = now_s() + 3.0;
        }
        return;
    }
    if (y >= TOOL_Y && y < TOOL_Y + TOOL_H) {
        if (x < 10 + hw)
            at_cmd(route == "speaker" ? "@route h" : "@route l");
        else
            at_cmd(mute == "1" ? "@mute 0" : "@mute 1");
        return;
    }
    if (y >= BTN_Y && y < BTN_Y + BTN_H) {
        at_cmd("AT+CHUP");
        busy_msg = "завершаю…";
        busy_until = now_s() + 3.0;
    }
}

int main(void)
{
    // одна копия: демон может позвать нас несколько раз за звонок
    int lock = open("/tmp/.callscreen.lock", O_CREAT | O_RDWR | O_CLOEXEC,
                    0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);

    state = readf("/run/phone/state");
    if (state != "ringing" && state != "active" && state != "dialing")
        return 0;                       // звонка уже нет

    // экран мог погаснуть по бездействию — зажигаем
    int bl = open("/sys/class/leds/lcd-backlight/brightness",
                  O_WRONLY | O_TRUNC);
    if (bl >= 0) {
        if (write(bl, "180", 3) < 0) { }
        close(bl);
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Вызов");
    XClassHint ch;
    ch.res_name = (char *)"callscreen";
    ch.res_class = (char *)"Callscreen";
    XSetClassHint(dpy, win, &ch);
    XSizeHints sh;
    sh.flags = PPosition | PSize | PMinSize | PMaxSize;
    sh.x = 0; sh.y = 0;
    sh.width = sh.min_width = sh.max_width = W;
    sh.height = sh.min_height = sh.max_height = H;
    XSetWMNormalHints(dpy, win, &sh);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask);
    XMapRaised(dpy, win);
    buf = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, buf, 0, NULL);
    xd = XftDrawCreate(dpy, buf, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));
    f_head = XftFontOpenName(dpy, scr, "DejaVu Sans:size=16");
    f_num = XftFontOpenName(dpy, scr, "DejaVu Sans:size=26:bold");
    f_btn = XftFontOpenName(dpy, scr, "DejaVu Sans:size=20:bold");
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=13:bold");
    f_tiny = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11:bold");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xffff, 0xffff, 0xffff, 0xffff};
    XRenderColor dc = {0x9a9a, 0xa4a4, 0xb0b0, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);

    int xfd = ConnectionNumber(dpy);
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose)
                draw();
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

        // отбой не подействовал — добиваем обычным ATH
        if (reject_at > 0 && now_s() - reject_at > 2.5) {
            reject_at = 0;
            if (readf("/run/phone/state") == "ringing")
                at_cmd("ATH");
        }
        if (now_s() > busy_until)
            busy_msg.clear();

        std::string st = readf("/run/phone/state");
        if (st == "idle" || st.empty())
            return 0;                   // вызов кончился — уходим
        if (st == "active" && state != "active")
            active_since = time(NULL);
        state = st;
        number = readf("/run/phone/number");
        route = readf("/run/phone/route");
        mute = readf("/run/phone/mute");
        // окно держим наверху: шторка или другое приложение могли
        // перекрыть его, пока телефон звонит
        XRaiseWindow(dpy, win);
        draw();
    }
    return 0;
}
