// Сообщения HTC HD2 — переписка, как на современном телефоне.
//
// Два экрана: список диалогов (собеседник, последняя реплика, время) и
// сама переписка пузырями — входящие слева, отправленные справа. Внизу
// переписки строка набора; клавиатура поднимается ТОЛЬКО когда в неё
// ткнули, при чтении и прокрутке её нет.
//
// С модемом напрямую не разговаривает: команды уходят демону phoned в
// /run/phone/cmd, ответы читаются из его журнала /run/phone/log.
// Кириллица приходит шестнадцатеричным потоком UCS-2, номера — ASCII-hex,
// длинные сообщения разбиты на части подряд — всё это разбирается тут.
// Отправленное модем не хранит, поэтому свои реплики пишем сами в
// /root/.sms-sent — без них переписки не получится.
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

static const int W = 480, H = 752, WIN_Y = 48;
static const unsigned long BG = 0x101418, KEYC = 0x1c2530, ACCENT = 0x2e7d32,
                           FIELD = 0x1c2530, OUTC = 0x2e5c34;
static const char *RUN = "/run/phone";

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_big, *f_txt, *f_small;
static XftColor c_fg, c_dim, c_warn, c_ok, c_err;


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

// ── сообщения и переписки ────────────────────────────────────────────
struct Msg {
    std::string peer;                  // номер собеседника
    std::string body;
    std::string when;                  // «ЧЧ:ММ» для показа
    long key;                          // для сортировки внутри переписки
    int out;                           // 1 — наше, 0 — входящее
};

struct Thread {
    std::string peer;
    std::vector<int> ids;              // указатели в all
};

static std::vector<Msg> all;
static std::vector<Thread> threads;

static const char *SENT_FILE = "/root/.sms-sent";

// «26/09/03,22:15:33+12» -> «22:15»
static std::string short_time(const std::string &w)
{
    size_t c = w.find(',');
    if (c == std::string::npos || c + 6 > w.size())
        return w;
    return w.substr(c + 1, 5);
}

// свои отправленные: «времяЧЧ:ММ<таб>номер<таб>текст», переводы строк \n
static void load_sent(void)
{
    std::string s = readf(SENT_FILE);
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find('\n', p);
        if (e == std::string::npos)
            e = s.size();
        std::string ln = s.substr(p, e - p);
        p = e + 1;
        size_t t1 = ln.find('\t');
        if (t1 == std::string::npos)
            continue;
        size_t t2 = ln.find('\t', t1 + 1);
        if (t2 == std::string::npos)
            continue;
        Msg m;
        m.when = ln.substr(0, t1);
        m.peer = ln.substr(t1 + 1, t2 - t1 - 1);
        std::string b = ln.substr(t2 + 1);
        std::string body;              // обратно разворачиваем переводы строк
        for (size_t i = 0; i < b.size(); i++) {
            if (b[i] == '\\' && i + 1 < b.size() && b[i + 1] == 'n') {
                body += '\n';
                i++;
            } else
                body += b[i];
        }
        m.body = body;
        m.out = 1;
        m.key = 1000000;               // свои — всегда после входящих
        all.push_back(m);
    }
}

static void save_sent(const std::string &peer, const std::string &body)
{
    int fd = open(SENT_FILE, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[8];
    strftime(hm, sizeof(hm), "%H:%M", &tm);
    std::string esc;
    for (size_t i = 0; i < body.size(); i++)
        if (body[i] == '\n')
            esc += "\\n";
        else if (body[i] != '\t')
            esc += body[i];
    std::string ln = std::string(hm) + "\t" + peer + "\t" + esc + "\n";
    if (write(fd, ln.c_str(), ln.size()) < 0) { }
    close(fd);
}

// номер в «человеческом» виде: последние 10 цифр с разделителями
static std::string pretty_num(const std::string &n)
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

// одинаковый ли это собеседник (сравниваем по последним 10 цифрам)
static std::string peer_key(const std::string &n)
{
    std::string d;
    for (size_t i = 0; i < n.size(); i++)
        if (isdigit((unsigned char)n[i]))
            d += n[i];
    if (d.size() > 10)
        d = d.substr(d.size() - 10);
    return d.empty() ? n : d;
}

static void build_threads(void)
{
    threads.clear();
    for (size_t i = 0; i < all.size(); i++) {
        std::string k = peer_key(all[i].peer);
        size_t j = 0;
        for (; j < threads.size(); j++)
            if (peer_key(threads[j].peer) == k)
                break;
        if (j == threads.size()) {
            Thread t;
            t.peer = all[i].peer;
            threads.push_back(t);
        }
        threads[j].ids.push_back((int)i);
    }
}

static std::string status_msg = "готово";

static void collect_messages(void)
{
    std::vector<std::string> lines = log_tail(8);
    std::vector<Msg> got;
    std::vector<int> idxs;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &ln = lines[i];
        if (ln.compare(0, 6, "+CMGL:") == 0) {
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
            m.out = 0;
            m.key = atoi(ln.c_str() + 6);
            m.peer = q.size() > 1 ? dehex(q[1]) : "?";
            m.when = q.size() > 2 ? short_time(q[2]) : "";
            got.push_back(m);
            idxs.push_back((int)m.key);
        } else if (!got.empty() && ln != "OK" && !ln.empty()) {
            got.back().body += dehex(ln);
        }
    }
    // длинное сообщение приходит кусками подряд от одного номера
    all.clear();
    for (size_t i = 0; i < got.size(); i++) {
        if (!all.empty() && !all.back().out &&
            peer_key(all.back().peer) == peer_key(got[i].peer) &&
            idxs[i] == (int)all.back().key + 1) {
            all.back().body += got[i].body;
            all.back().key = idxs[i];
        } else
            all.push_back(got[i]);
    }
    load_sent();
    build_threads();
    char st[64];
    snprintf(st, sizeof(st), "диалогов: %d", (int)threads.size());
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

// перенос по ширине с учётом UTF-8 и переводов строк
static std::vector<std::string> wrap(XftFont *fn, const std::string &s,
                                     int width)
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
        if (tw(fn, (cur + ch).c_str()) > width && !cur.empty()) {
            // переносим по последнему пробелу, чтобы слова не рвались;
            // если слово само шире строки — рвём его, деваться некуда
            size_t sp = cur.rfind(' ');
            if (sp != std::string::npos && sp > 0) {
                std::string tail = cur.substr(sp + 1);
                out.push_back(cur.substr(0, sp));
                cur = tail + ch;
            } else {
                out.push_back(cur);
                cur = ch;
            }
        } else
            cur += ch;
    }
    out.push_back(cur);
    return out;
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

// ── экраны ───────────────────────────────────────────────────────────
enum { SCR_LIST, SCR_THREAD, SCR_NEW };
static int screen = SCR_LIST;
static int cur = -1;                   // открытая переписка
static std::string compose, new_num;
static int scroll = 0;                 // прокрутка списка/переписки
static int kbd_up = 0;                 // поднята ли клавиатура
static time_t send_at_time = 0, refresh_at_time = 0;

static const int HEAD_H = 56;
static const int ROW_H = 84;           // строка списка диалогов
static const int SEND_W = 84, BAR_H = 62;
// когда клавиатура поднята, она закрывает низ окна (экранно с 513),
// поэтому строку набора поднимаем над ней
static int bar_y(void) { return kbd_up ? 400 : H - BAR_H - 10; }

// Клавиатура: keysd поднимает её по метке, а не по классу окна — иначе
// она вылезала бы и при простом чтении переписки. В метке наш pid,
// чтобы keysd убрал её, если мы вдруг закроемся.
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
    draw_head("Сообщения", 0);
    // «Обновить» — справа в шапке
    const char *r = "⟳";
    fill(0x27313d, W - 58, 8, 48, 40);
    text(f_big, &c_fg, W - 58 + (48 - tw(f_big, r)) / 2, 36, r);

    int y0 = HEAD_H + 4, bottom = H - 74;
    if (threads.empty())
        text(f_txt, &c_dim, 20, y0 + 40, "переписок пока нет");
    for (size_t i = (size_t)scroll; i < threads.size(); i++) {
        int y = y0 + ((int)i - scroll) * ROW_H;
        if (y + ROW_H > bottom)
            break;
        fill(KEYC, 10, y, W - 20, ROW_H - 6);
        const Msg &last = all[threads[i].ids.back()];
        std::string nm = pretty_num(threads[i].peer);
        int time_w = tw(f_small, last.when.c_str());
        text(f_big, &c_fg, 24, y + 32,
             cut(f_big, nm, W - 60 - time_w).c_str());
        text(f_small, &c_dim, W - 24 - time_w, y + 30, last.when.c_str());
        std::string pv = (last.out ? "вы: " : "") + last.body;
        for (size_t k = 0; k < pv.size(); k++)
            if (pv[k] == '\n')
                pv[k] = ' ';
        text(f_txt, &c_dim, 24, y + 60, cut(f_txt, pv, W - 48).c_str());
    }

    fill(ACCENT, 10, H - 68, W - 20, 58);
    const char *n = "Написать";
    text(f_big, &c_fg, (W - tw(f_big, n)) / 2, H - 30, n);
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

// высота пузыря в точках
static int bubble_h(const std::vector<std::string> &lines)
{
    return 16 + (int)lines.size() * 24 + 18;
}

static void draw_thread(void)
{
    fill(BG, 0, 0, W, H);
    const Thread &t = threads[cur];

    int top = HEAD_H + 6, bot = bar_y() - 8;
    int maxw = 320;
    // раскладываем снизу вверх: свежие реплики всегда видны
    int y = bot;
    for (int i = (int)t.ids.size() - 1 - scroll; i >= 0; i--) {
        if (i >= (int)t.ids.size())
            continue;
        const Msg &m = all[t.ids[i]];
        std::vector<std::string> lines = wrap(f_txt, m.body, maxw - 28);
        int bh = bubble_h(lines);
        y -= bh + 8;
        if (y + bh < top)
            break;
        int bw = 0;
        for (size_t k = 0; k < lines.size(); k++) {
            int lw = tw(f_txt, lines[k].c_str());
            if (lw > bw)
                bw = lw;
        }
        bw += 28;
        if (bw < 120)
            bw = 120;
        int x = m.out ? W - 12 - bw : 12;
        fill(m.out ? OUTC : KEYC, x, y, bw, bh);
        for (size_t k = 0; k < lines.size(); k++)
            text(f_txt, &c_fg, x + 14, y + 30 + (int)k * 24,
                 lines[k].c_str());
        text(f_small, &c_dim, x + bw - 14 - tw(f_small, m.when.c_str()),
             y + bh - 6, m.when.c_str());
    }

    // шапка поверх пузырей: длинное сообщение иначе заезжает на неё
    draw_head(cut(f_big, pretty_num(t.peer), W - 160).c_str(), 1);

    // строка набора
    int by = bar_y();
    fill(FIELD, 10, by, W - 20 - SEND_W - 6, BAR_H);
    if (kbd_up) {                      // рамка показывает, что ввод активен
        XSetForeground(dpy, gc, 0x3a5578);
        XDrawRectangle(dpy, buf, gc, 10, by, W - 21 - SEND_W - 6, BAR_H - 1);
    }
    std::string shown = compose.empty() ? "Сообщение…" : compose;
    std::vector<std::string> cl = wrap(f_txt, shown, W - 60 - SEND_W);
    // в строке видно две последние строки текста
    size_t first = cl.size() > 2 ? cl.size() - 2 : 0;
    for (size_t k = first; k < cl.size(); k++)
        text(f_txt, compose.empty() ? &c_dim : &c_fg, 22,
             by + 26 + (int)(k - first) * 24, cl[k].c_str());

    fill(compose.empty() ? KEYC : ACCENT, W - 10 - SEND_W, by, SEND_W, BAR_H);
    const char *s = "▶";
    text(f_big, &c_fg, W - 10 - SEND_W + (SEND_W - tw(f_big, s)) / 2,
         by + 40, s);
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void draw_new(void)
{
    fill(BG, 0, 0, W, H);
    draw_head("Новое сообщение", 1);
    text(f_small, &c_dim, 14, HEAD_H + 34, "Номер получателя");
    fill(FIELD, 10, HEAD_H + 46, W - 20, 60);
    XSetForeground(dpy, gc, 0x3a5578);
    XDrawRectangle(dpy, buf, gc, 10, HEAD_H + 46, W - 21, 59);
    text(f_big, &c_fg, 22, HEAD_H + 86, new_num.c_str());
    fill(new_num.empty() ? KEYC : ACCENT, 10, HEAD_H + 126, W - 20, 60);
    const char *n = "Дальше";
    text(f_big, &c_fg, (W - tw(f_big, n)) / 2, HEAD_H + 166, n);
    text(f_small, &c_dim, 14, HEAD_H + 216,
         "номер вводится экранной клавиатурой, слой 123");
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void draw(void)
{
    if (screen == SCR_LIST)
        draw_list();
    else if (screen == SCR_THREAD)
        draw_thread();
    else
        draw_new();
}

// ── действия ─────────────────────────────────────────────────────────
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
}

static void do_send(void)
{
    if (cur < 0 || compose.empty())
        return;
    std::string peer = threads[cur].peer;
    at_cmd("AT+CMGS=\"" + peer + "\"");
    usleep(1000000);
    at_cmd(compose + "\x1a");          // текст и Ctrl-Z
    save_sent(peer, compose);
    // сразу показываем свою реплику, не дожидаясь модема
    Msg m;
    m.peer = peer;
    m.body = compose;
    m.out = 1;
    m.key = 1000000;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[8];
    strftime(hm, sizeof(hm), "%H:%M", &tm);
    m.when = hm;
    all.push_back(m);
    threads[cur].ids.push_back((int)all.size() - 1);
    compose.clear();
    scroll = 0;
    send_at_time = time(NULL);
    status_msg = "отправляется…";
}

static void open_thread(const std::string &peer)
{
    std::string k = peer_key(peer);
    for (size_t i = 0; i < threads.size(); i++)
        if (peer_key(threads[i].peer) == k) {
            cur = (int)i;
            screen = SCR_THREAD;
            scroll = 0;
            return;
        }
    Thread t;                          // переписки ещё нет — заводим пустую
    t.peer = peer;
    threads.push_back(t);
    cur = (int)threads.size() - 1;
    screen = SCR_THREAD;
    scroll = 0;
}

static void click(int x, int y)
{
    if (screen == SCR_LIST) {
        if (y < HEAD_H) {
            if (x > W - 60)
                do_refresh();
            return;
        }
        if (y >= H - 68) {
            screen = SCR_NEW;
            new_num.clear();
            kbd_show();
            return;
        }
        int i = scroll + (y - HEAD_H - 4) / ROW_H;
        if (i >= 0 && i < (int)threads.size()) {
            cur = i;
            screen = SCR_THREAD;
            scroll = 0;
        }
        return;
    }
    if (screen == SCR_NEW) {
        if (y < HEAD_H) {
            screen = SCR_LIST;
            kbd_hide();
            return;
        }
        if (y >= HEAD_H + 46 && y < HEAD_H + 106) {
            kbd_show();
            return;
        }
        if (y >= HEAD_H + 126 && y < HEAD_H + 186 && !new_num.empty()) {
            open_thread(new_num);
            kbd_hide();
        }
        return;
    }
    // переписка
    if (y < HEAD_H) {
        if (x < 140) {
            screen = SCR_LIST;
            kbd_hide();
            scroll = 0;
        }
        return;
    }
    int by = bar_y();
    if (y >= by && y < by + BAR_H) {
        if (x >= W - 10 - SEND_W) {
            do_send();
            kbd_hide();
        } else
            kbd_show();                // тап по строке — только тут клавиатура
        return;
    }
}

static void key_in(XKeyEvent *e)
{
    char b[16];
    KeySym ks;
    int n = XLookupString(e, b, sizeof(b) - 1, &ks, NULL);
    if (screen == SCR_LIST)
        return;
    std::string &dst = (screen == SCR_NEW) ? new_num : compose;
    if (ks == XK_BackSpace) {
        while (!dst.empty() && ((unsigned char)dst.back() & 0xc0) == 0x80)
            dst.erase(dst.size() - 1);
        if (!dst.empty())
            dst.erase(dst.size() - 1);
    } else if (ks == XK_Return) {
        if (screen == SCR_NEW) {
            if (!new_num.empty()) {
                open_thread(new_num);
                kbd_hide();
            }
        } else
            dst += "\n";
    } else if (n > 0) {
        b[n] = 0;
        dst += b;
    } else if (ks >= 0x01000000) {     // юникод из экранной клавиатуры
        dst += cp_to_utf8(ks & 0xffffff);
    }
    draw();
}

int main(int argc, char **argv)
{
    // «phone-sms new» открывается сразу свежей перепиской: по
    // уведомлению о новом сообщении нужна она, а не общий список
    int open_newest = (argc > 1 && !strcmp(argv[1], "new"));
    int lock = open("/run/.sms.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
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
    // класса «sms» тут больше НЕТ: по нему keysd поднимал клавиатуру на
    // любое окно сообщений, и она лезла даже при чтении переписки
    ch.res_name = (char *)"smsapp";
    ch.res_class = (char *)"Smsapp";
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
    XRenderColor yc = {0xffff, 0xa7a7, 0x2626, 0xffff};
    XRenderColor gc2 = {0x6666, 0xbbbb, 0x6a6a, 0xffff};
    XRenderColor rc = {0xefef, 0x5353, 0x5050, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);
    XftColorAllocValue(dpy, vis, cm, &yc, &c_warn);
    XftColorAllocValue(dpy, vis, cm, &gc2, &c_ok);
    XftColorAllocValue(dpy, vis, cm, &rc, &c_err);

    load_sent();
    build_threads();
    do_refresh();
    draw();

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
                int step = (screen == SCR_THREAD) ? 60 : ROW_H;
                int ns = scroll_at_press + (screen == SCR_THREAD
                                            ? dy / step : -dy / step);
                int maxs = 0;
                if (screen == SCR_LIST)
                    maxs = (int)threads.size() - 6;
                else if (screen == SCR_THREAD && cur >= 0)
                    maxs = (int)threads[cur].ids.size() - 1;
                if (maxs < 0)
                    maxs = 0;
                if (ns < 0) ns = 0;
                if (ns > maxs) ns = maxs;
                if (ns != scroll) {
                    scroll = ns;
                    draw();
                }
            } else if (e.type == ButtonRelease) {
                if (!moved) {          // прокрутка не должна срабатывать как тап
                    click(e.xbutton.x, e.xbutton.y);
                    draw();
                }
            } else if (e.type == KeyPress)
                key_in(&e.xkey);
            else if (e.type == ClientMessage &&
                     (Atom)e.xclient.data.l[0] == wm_del) {
                kbd_hide();            // уходим — клавиатуру за собой убираем
                return 0;
            }
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {1, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);
        time_t now = time(NULL);
        if (send_at_time && now - send_at_time >= 6) {
            send_at_time = 0;
            std::vector<std::string> r = log_tail(10);
            int ok = 0, err = 0;
            for (size_t i = 0; i < r.size(); i++) {
                if (r[i].compare(0, 6, "+CMGS:") == 0)
                    ok = 1;
                if (r[i].find("ERROR") != std::string::npos)
                    err = 1;
            }
            status_msg = ok ? "отправлено"
                            : (err ? "ошибка отправки" : "ответа нет");
            draw();
        }
        if (refresh_at_time && now - refresh_at_time >= 4) {
            refresh_at_time = 0;
            int keep = cur;
            std::string peer = (cur >= 0 && cur < (int)threads.size())
                               ? threads[cur].peer : "";
            collect_messages();
            if (open_newest && !threads.empty()) {
                // самая свежая переписка — та, где последним пришло
                // входящее; при равенстве берём последнюю в списке
                int best = -1;
                for (size_t i = 0; i < threads.size(); i++)
                    if (!all[threads[i].ids.back()].out)
                        best = (int)i;
                if (best < 0)
                    best = (int)threads.size() - 1;
                cur = best;
                screen = SCR_THREAD;
                scroll = 0;
                open_newest = 0;
                draw();
                continue;
            }
            if (!peer.empty()) {       // после перечитывания остаёмся в том же
                cur = -1;              // диалоге, а не вываливаемся в список
                for (size_t i = 0; i < threads.size(); i++)
                    if (peer_key(threads[i].peer) == peer_key(peer))
                        cur = (int)i;
                if (cur < 0) {
                    screen = SCR_LIST;
                    cur = keep;
                }
            }
            draw();
        }
    }
    return 0;
}
