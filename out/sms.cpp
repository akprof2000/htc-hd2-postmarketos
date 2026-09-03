// SMS для HTC HD2 — нативная замена phone-sms на Python/Tk.
//
// С модемом напрямую не разговаривает: команды уходят демону phoned в
// /run/phone/cmd, ответы читаются из его журнала /run/phone/log.
// Кириллица приходит шестнадцатеричным потоком UCS-2, номера — ASCII-hex,
// длинные сообщения разбиты на части подряд — всё это разбирается тут.
//
// Ввод текста — экранной клавиатурой: своё поле ввода получает символы
// через XTEST, как обычное окно.
//
// Сборка: g++ -O2 sms.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o phone-sms

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

static const int W = 480, H = 776, WIN_Y = 24;
static const unsigned long BG = 0x101418, KEYC = 0x1c2530, ACCENT = 0x2e7d32,
                           FIELD = 0x1c2530;
static const char *RUN = "/run/phone";

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_big, *f_txt, *f_small;
static XftColor c_fg, c_dim, c_warn, c_ok, c_err;

static std::string to_num, body, status_msg = "готово";
static int focus_body = 1;             // куда попадают набранные символы
static std::vector<std::string> msgs;  // готовые строки для показа
static int scroll = 0;
static time_t send_at_time = 0, refresh_at_time = 0;

// ── система ──────────────────────────────────────────────────────────
static std::string readf(const std::string &p)
{
    int fd = open(p.c_str(), O_RDONLY);
    if (fd < 0)
        return "";
    std::string out;
    char b[4096];
    ssize_t n;
    while ((n = read(fd, b, sizeof(b))) > 0)
        out.append(b, n);
    close(fd);
    return out;
}

static void at_cmd(const std::string &s)
{
    int fd = open((std::string(RUN) + "/cmd").c_str(),
                  O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return;
    std::string l = s + "\n";
    if (write(fd, l.c_str(), l.size()) < 0) { }
    close(fd);
}

// строки журнала демона за последние N секунд (формат "ЧЧ:ММ:СС <- текст")
static std::vector<std::string> log_tail(int seconds)
{
    std::vector<std::string> out;
    std::string all = readf(std::string(RUN) + "/log");
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int now_s = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    size_t p = 0;
    while (p < all.size()) {
        size_t e = all.find('\n', p);
        if (e == std::string::npos)
            e = all.size();
        std::string ln = all.substr(p, e - p);
        p = e + 1;
        if (ln.size() < 12)
            continue;
        int hh = atoi(ln.substr(0, 2).c_str());
        int mm = atoi(ln.substr(3, 2).c_str());
        int ss = atoi(ln.substr(6, 2).c_str());
        int t = hh * 3600 + mm * 60 + ss;
        if (now_s - t > seconds || now_s < t)
            continue;
        size_t a = ln.find("<- ");
        if (a != std::string::npos)
            out.push_back(ln.substr(a + 3));
    }
    return out;
}

// ── разбор шестнадцатеричного текста ─────────────────────────────────
static int is_hex(const std::string &s)
{
    if (s.empty() || s.size() % 2)
        return 0;
    for (size_t i = 0; i < s.size(); i++)
        if (!isxdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

static std::string cp_to_utf8(unsigned cp)
{
    std::string r;
    if (cp < 0x80)
        r += (char)cp;
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

static std::string dehex(std::string s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    if (!is_hex(s))
        return s;
    std::vector<unsigned char> raw;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        raw.push_back((unsigned char)strtol(s.substr(i, 2).c_str(), NULL, 16));
    // UCS-2: старшие байты в основном 0x00 (латиница) или 0x04 (кириллица)
    int hi_ok = 0;
    for (size_t i = 0; i < raw.size(); i += 2)
        if (raw[i] == 0 || raw[i] == 4)
            hi_ok++;
    if (raw.size() % 2 == 0 && hi_ok >= (int)raw.size() / 4) {
        std::string out;
        for (size_t i = 0; i + 1 < raw.size(); i += 2) {
            unsigned cp = (raw[i] << 8) | raw[i + 1];
            if (cp != '\r')
                out += cp_to_utf8(cp);
        }
        return out;
    }
    int ascii = 1;
    for (size_t i = 0; i < raw.size(); i++)
        if (raw[i] < 32 || raw[i] > 126)
            ascii = 0;
    if (ascii)
        return std::string((char *)raw.data(), raw.size());
    return s;
}

// ── чтение списка сообщений ──────────────────────────────────────────
struct Msg { std::string from, when, body; int idx; };

static void collect_messages(void)
{
    std::vector<std::string> lines = log_tail(8);
    std::vector<Msg> got;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &ln = lines[i];
        if (ln.compare(0, 6, "+CMGL:") == 0) {
            // +CMGL: idx,"стат","номер",,"дата"
            std::vector<std::string> q;      // всё в кавычках
            size_t p = 0;
            while ((p = ln.find('"', p)) != std::string::npos) {
                size_t e = ln.find('"', p + 1);
                if (e == std::string::npos)
                    break;
                q.push_back(ln.substr(p + 1, e - p - 1));
                p = e + 1;
            }
            Msg m;
            m.idx = atoi(ln.c_str() + 6);
            m.from = q.size() > 1 ? dehex(q[1]) : "?";
            m.when = q.size() > 2 ? q[2] : "";
            got.push_back(m);
        } else if (!got.empty() && ln != "OK" && !ln.empty()) {
            got.back().body += dehex(ln);
        }
    }
    // длинное сообщение приходит кусками подряд от одного номера
    msgs.clear();
    std::vector<Msg> merged;
    for (size_t i = 0; i < got.size(); i++) {
        if (!merged.empty() && merged.back().from == got[i].from &&
            got[i].idx == merged.back().idx + 1) {
            merged.back().body += got[i].body;
            merged.back().idx = got[i].idx;
        } else
            merged.push_back(got[i]);
    }
    for (size_t i = 0; i < merged.size(); i++) {
        msgs.push_back(merged[i].from + "  ·  " + merged[i].when);
        // текст разбиваем на строки по ширине окна
        std::string b = merged[i].body;
        size_t p = 0;
        while (p < b.size()) {
            size_t n = 46;
            if (p + n > b.size())
                n = b.size() - p;
            else {                      // не рвём UTF-8 посередине
                while (n > 1 && ((unsigned char)b[p + n] & 0xc0) == 0x80)
                    n--;
            }
            msgs.push_back("  " + b.substr(p, n));
            p += n;
        }
        msgs.push_back("");
    }
    if (msgs.empty())
        msgs.push_back("нет сообщений");
    char st[64];
    snprintf(st, sizeof(st), "сообщений: %d", (int)merged.size());
    status_msg = st;
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

static const int TO_Y = 40, TO_H = 44;
static const int BODY_Y = 112, BODY_H = 120;
static const int BAR_Y = 248, BAR_H = 52;
static const int ST_Y = 322;
static const int LIST_Y = 340;

static void draw(void)
{
    fill(BG, 0, 0, W, H);
    text(f_small, &c_dim, 12, 24, "Кому (номер)");
    fill(FIELD, 10, TO_Y, W - 20, TO_H);
    if (focus_body == 0)               // поле в фокусе — светлая рамка
        XSetForeground(dpy, gc, 0x3a5578), XDrawRectangle(dpy, buf, gc, 10,
                                                          TO_Y, W - 21,
                                                          TO_H - 1);
    text(f_txt, &c_fg, 20, TO_Y + 30, to_num.c_str());

    text(f_small, &c_dim, 12, BODY_Y - 14, "Текст");
    fill(FIELD, 10, BODY_Y, W - 20, BODY_H);
    if (focus_body)
        XSetForeground(dpy, gc, 0x3a5578), XDrawRectangle(dpy, buf, gc, 10,
                                                          BODY_Y, W - 21,
                                                          BODY_H - 1);
    // текст в поле — переносим по ширине
    {
        std::string b = body;
        int y = BODY_Y + 26;
        size_t p = 0;
        while (p < b.size() && y < BODY_Y + BODY_H) {
            size_t n = 34;
            if (p + n > b.size())
                n = b.size() - p;
            else
                while (n > 1 && ((unsigned char)b[p + n] & 0xc0) == 0x80)
                    n--;
            text(f_txt, &c_fg, 20, y, b.substr(p, n).c_str());
            p += n;
            y += 26;
        }
    }

    int bw = (W - 20 - 2 * 6) / 3;
    fill(ACCENT, 10, BAR_Y, bw, BAR_H);
    const char *s1 = "Отправить";
    text(f_small, &c_fg, 10 + (bw - tw(f_small, s1)) / 2, BAR_Y + 32, s1);
    fill(KEYC, 10 + bw + 6, BAR_Y, bw, BAR_H);
    const char *s2 = "Обновить";
    text(f_small, &c_fg, 10 + bw + 6 + (bw - tw(f_small, s2)) / 2,
         BAR_Y + 32, s2);
    fill(KEYC, 10 + 2 * (bw + 6), BAR_Y, bw, BAR_H);
    const char *s3 = "Клавиатура";
    text(f_small, &c_fg, 10 + 2 * (bw + 6) + (bw - tw(f_small, s3)) / 2,
         BAR_Y + 32, s3);

    text(f_small, &c_dim, 12, ST_Y, status_msg.c_str());

    // список сообщений
    fill(0x161d26, 10, LIST_Y, W - 20, H - LIST_Y - 10);
    int y = LIST_Y + 22;
    for (size_t i = (size_t)scroll; i < msgs.size() && y < H - 16; i++) {
        int header = !msgs[i].empty() && msgs[i][0] != ' ';
        text(f_small, header ? &c_ok : &c_fg, 18, y, msgs[i].c_str());
        y += 20;
    }
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

// ── действия ─────────────────────────────────────────────────────────
static void do_send(void)
{
    if (to_num.empty() || body.empty()) {
        status_msg = "нужен номер и текст";
        draw();
        return;
    }
    at_cmd("AT+CMGS=\"" + to_num + "\"");
    usleep(1000000);
    at_cmd(body + "\x1a");             // текст и Ctrl-Z
    status_msg = "отправляется…";
    send_at_time = time(NULL);
    draw();
}

static void check_sent(void)
{
    std::vector<std::string> r = log_tail(10);
    int ok = 0, err = 0;
    for (size_t i = 0; i < r.size(); i++) {
        if (r[i].compare(0, 6, "+CMGS:") == 0)
            ok = 1;
        if (r[i].find("ERROR") != std::string::npos)
            err = 1;
    }
    if (ok) {
        status_msg = "отправлено";
        body.clear();
    } else
        status_msg = err ? "ошибка отправки" : "ответа нет";
    draw();
}

static void do_refresh(void)
{
    int fd = open((std::string(RUN) + "/sms_new").c_str(),
                  O_WRONLY | O_TRUNC);  // входящие просмотрены
    if (fd >= 0)
        close(fd);
    status_msg = "читаю сообщения…";
    at_cmd("AT+CMGF=1");
    usleep(400000);
    at_cmd("AT+CMGL=\"ALL\"");
    refresh_at_time = time(NULL);
    draw();
}

static void click(int x, int y)
{
    if (y >= TO_Y && y < TO_Y + TO_H) {
        focus_body = 0;
        draw();
        return;
    }
    if (y >= BODY_Y && y < BODY_Y + BODY_H) {
        focus_body = 1;
        draw();
        return;
    }
    int bw = (W - 20 - 2 * 6) / 3;
    if (y >= BAR_Y && y < BAR_Y + BAR_H) {
        if (x < 10 + bw)
            do_send();
        else if (x < 10 + 2 * bw + 6)
            do_refresh();
        else
            system("DISPLAY=:0 /usr/local/bin/kbd >/dev/null 2>&1 &");
        return;
    }
    if (y >= LIST_Y) {                 // тап по списку прокручивает
        scroll += (y > (LIST_Y + H) / 2) ? 5 : -5;
        if (scroll < 0)
            scroll = 0;
        if (scroll > (int)msgs.size() - 3)
            scroll = (int)msgs.size() > 3 ? (int)msgs.size() - 3 : 0;
        draw();
    }
}

static void key_in(XKeyEvent *e)
{
    char b[16];
    KeySym ks;
    int n = XLookupString(e, b, sizeof(b) - 1, &ks, NULL);
    std::string &dst = focus_body ? body : to_num;
    if (ks == XK_BackSpace) {
        while (!dst.empty() &&
               ((unsigned char)dst.back() & 0xc0) == 0x80)  // хвост UTF-8
            dst.erase(dst.size() - 1);
        if (!dst.empty())
            dst.erase(dst.size() - 1);
    } else if (ks == XK_Return) {
        if (focus_body)
            dst += "\n";
        else
            focus_body = 1;
    } else if (n > 0) {
        b[n] = 0;
        dst += b;
    } else if (ks >= 0x01000000) {     // юникод из экранной клавиатуры
        dst += cp_to_utf8(ks & 0xffffff);
    }
    draw();
}

int main(void)
{
    int lock = open("/tmp/.sms.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "нет доступа к X\n");
        return 1;
    }
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Сообщения");
    XClassHint ch;
    ch.res_name = (char *)"sms";       // по этому классу keysd поднимает
    ch.res_class = (char *)"Sms";      // экранную клавиатуру
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
    f_big = XftFontOpenName(dpy, scr, "DejaVu Sans:size=15:bold");
    f_txt = XftFontOpenName(dpy, scr, "DejaVu Sans:size=13");
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XRenderColor yc = {0xffff, 0xa7a7, 0x2626, 0xffff};
    XRenderColor gc2 = {0x6666, 0xbbbb, 0x6a6a, 0xffff};
    XRenderColor rc = {0xefef, 0x5353, 0x5050, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);
    XftColorAllocValue(dpy, vis, cm, &yc, &c_warn);
    XftColorAllocValue(dpy, vis, cm, &gc2, &c_ok);
    XftColorAllocValue(dpy, vis, cm, &rc, &c_err);

    do_refresh();
    int xfd = ConnectionNumber(dpy);
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose)
                draw();
            else if (e.type == ButtonPress)
                click(e.xbutton.x, e.xbutton.y);
            else if (e.type == KeyPress)
                key_in(&e.xkey);
            else if (e.type == ClientMessage &&
                     (Atom)e.xclient.data.l[0] == wm_del)
                return 0;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {1, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);
        time_t now = time(NULL);
        if (send_at_time && now - send_at_time >= 6) {
            send_at_time = 0;
            check_sent();
        }
        if (refresh_at_time && now - refresh_at_time >= 4) {
            refresh_at_time = 0;
            collect_messages();
            draw();
        }
    }
    return 0;
}
