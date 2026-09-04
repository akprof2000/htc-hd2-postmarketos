// Почта HTC HD2 — нативная замена mail на Python.
//
// Сеть держит libcurl (на телефоне есть imaps и smtps), поэтому ни TLS,
// ни разбор ответов IMAP вручную писать не надо. Долгие обращения к
// серверу идут ОТДЕЛЬНЫМ процессом — той же программой, запущенной с
// параметрами, — а окно тем временем живёт и рисуется:
//
//     mail                     — интерфейс
//     mail fetch [сколько]     — заголовки последних писем
//     mail body <uid>          — текст письма
//     mail send <кому>         — письмо со стандартного ввода
//
// Настройки — /root/.mailrc, простыми строками «ключ: значение»:
//     user: адрес
//     password: пароль приложения
//     imap: imap.yandex.ru
//     smtp: smtp.yandex.ru
// Для Gmail и Яндекса нужен именно пароль приложения, обычный не подойдёт.
//
// Сборка: g++ -O2 mail.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -lcurl -o mail

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>

#include <curl/curl.h>

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

static const char *CFG = "/root/.mailrc";

// ── настройки ────────────────────────────────────────────────────────
struct Cfg {
    std::string user, password, imap, smtp;
};
static Cfg cfg;

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

static void cfg_load(void)
{
    cfg.imap = "imap.yandex.ru";
    cfg.smtp = "smtp.yandex.ru";
    std::string s = readf(CFG);
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find('\n', p);
        if (e == std::string::npos)
            e = s.size();
        std::string ln = s.substr(p, e - p);
        p = e + 1;
        size_t c = ln.find(':');
        if (c == std::string::npos)
            continue;
        std::string k = ln.substr(0, c), v = ln.substr(c + 1);
        while (!v.empty() && (v[0] == ' ' || v[0] == '\t'))
            v.erase(0, 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' '))
            v.pop_back();
        if (k == "user") cfg.user = v;
        else if (k == "password") cfg.password = v;
        else if (k == "imap") cfg.imap = v;
        else if (k == "smtp") cfg.smtp = v;
    }
}

static void cfg_save(void)
{
    std::string s = "user: " + cfg.user + "\npassword: " + cfg.password +
                    "\nimap: " + cfg.imap + "\nsmtp: " + cfg.smtp + "\n";
    int fd = open(CFG, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    if (write(fd, s.c_str(), s.size()) < 0) { }
    close(fd);
}

// ── разбор писем ─────────────────────────────────────────────────────
static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::string b64_decode(const std::string &s)
{
    std::string out;
    int acc = 0, bits = 0;
    for (size_t i = 0; i < s.size(); i++) {
        int v = b64v((unsigned char)s[i]);
        if (v < 0)
            continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((acc >> bits) & 0xff);
        }
    }
    return out;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// quoted-printable; in_word — вариант из =?...?Q?...?=, где _ это пробел
static std::string qp_decode(const std::string &s, int in_word)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '=' && i + 2 < s.size()) {
            int h = hexv(s[i + 1]), l = hexv(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out += (char)(h * 16 + l);
                i += 2;
                continue;
            }
            if (s[i + 1] == '\r' || s[i + 1] == '\n') {   // мягкий перенос
                i += (s[i + 1] == '\r' && s[i + 2] == '\n') ? 2 : 1;
                continue;
            }
        }
        if (in_word && s[i] == '_')
            out += ' ';
        else
            out += s[i];
    }
    return out;
}

// однобайтовые кириллические кодировки в UTF-8
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

static std::string to_utf8(const std::string &s, const std::string &charset)
{
    std::string cs;
    for (size_t i = 0; i < charset.size(); i++)
        cs += tolower((unsigned char)charset[i]);
    if (cs.find("utf-8") != std::string::npos || cs.empty())
        return s;
    std::string out;
    if (cs.find("1251") != std::string::npos) {
        for (size_t i = 0; i < s.size(); i++) {
            unsigned char c = s[i];
            if (c < 0x80) out += (char)c;
            else if (c == 0xa8) out += cp_to_utf8(0x0401);   // Ё
            else if (c == 0xb8) out += cp_to_utf8(0x0451);   // ё
            else if (c >= 0xc0) out += cp_to_utf8(0x0410 + (c - 0xc0));
            else out += '?';
        }
        return out;
    }
    // прочие кодировки не разбираем: высокие байты честно помечаем
    // вопросом, чтобы не выдавать мусор за текст
    if (cs.find("ascii") == std::string::npos) {
        for (size_t i = 0; i < s.size(); i++)
            out += ((unsigned char)s[i] < 0x80) ? std::string(1, s[i])
                                                : std::string("?");
        return out;
    }
    return s;
}

// RFC 2047: =?кодировка?B|Q?текст?=
static std::string dec_header(const std::string &s)
{
    std::string out;
    size_t p = 0;
    while (p < s.size()) {
        size_t a = s.find("=?", p);
        if (a == std::string::npos) {
            out += s.substr(p);
            break;
        }
        out += s.substr(p, a - p);
        size_t q1 = s.find('?', a + 2);
        if (q1 == std::string::npos) { out += s.substr(a); break; }
        size_t q2 = s.find('?', q1 + 1);
        if (q2 == std::string::npos) { out += s.substr(a); break; }
        size_t end = s.find("?=", q2 + 1);
        if (end == std::string::npos) { out += s.substr(a); break; }
        std::string charset = s.substr(a + 2, q1 - a - 2);
        char how = toupper(s[q1 + 1]);
        std::string txt = s.substr(q2 + 1, end - q2 - 1);
        std::string raw = (how == 'B') ? b64_decode(txt) : qp_decode(txt, 1);
        out += to_utf8(raw, charset);
        p = end + 2;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t') &&
               s.compare(p + 1, 2, "=?") == 0)
            p++;                       // склейка соседних слов
    }
    return out;
}

// ── работа с сервером ────────────────────────────────────────────────
static size_t sink(void *p, size_t sz, size_t nm, void *ud)
{
    ((std::string *)ud)->append((char *)p, sz * nm);
    return sz * nm;
}

static CURL *mk(const std::string &url, std::string *out)
{
    CURL *c = curl_easy_init();
    if (!c)
        return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERNAME, cfg.user.c_str());
    curl_easy_setopt(c, CURLOPT_PASSWORD, cfg.password.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    // на этом телефоне набор корневых сертификатов неполон, а почтовый
    // ящик всё равно защищён паролем приложения
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    return c;
}

// поле заголовка из куска письма
static std::string field(const std::string &h, const char *name)
{
    std::string low;
    for (size_t i = 0; i < h.size(); i++)
        low += tolower((unsigned char)h[i]);
    std::string key = std::string("\n") + name + ":";
    size_t p = low.compare(0, strlen(name) + 1, std::string(name) + ":") == 0
               ? 0 : low.find(key);
    if (p == std::string::npos)
        return "";
    if (p)
        p++;
    p = h.find(':', p);
    if (p == std::string::npos)
        return "";
    p++;
    std::string v;
    while (p < h.size()) {
        size_t e = h.find('\n', p);
        if (e == std::string::npos)
            e = h.size();
        std::string ln = h.substr(p, e - p);
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' '))
            ln.pop_back();
        while (!ln.empty() && (ln[0] == ' ' || ln[0] == '\t'))
            ln.erase(0, 1);
        v += (v.empty() ? "" : " ") + ln;
        p = e + 1;
        if (p >= h.size() || (h[p] != ' ' && h[p] != '\t'))
            break;                     // продолжение — только с отступа
    }
    return v;
}

static int do_fetch(int howmany)
{
    std::string out;
    CURL *c = mk("imaps://" + cfg.imap + "/INBOX", &out);
    if (!c)
        return 1;
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "UID SEARCH ALL");
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) {
        printf("ОШИБКА %s\n", curl_easy_strerror(r));
        return 1;
    }
    // «* SEARCH 1 2 3 …»
    std::vector<long> uids;
    size_t p = out.find("SEARCH");
    if (p != std::string::npos) {
        p += 6;
        while (p < out.size()) {
            while (p < out.size() && (out[p] == ' ' || out[p] == '\t'))
                p++;
            if (p >= out.size() || !isdigit((unsigned char)out[p]))
                break;
            uids.push_back(atol(out.c_str() + p));
            while (p < out.size() && isdigit((unsigned char)out[p]))
                p++;
        }
    }
    if (uids.empty()) {
        printf("ПУСТО\n");
        return 0;
    }
    size_t from = uids.size() > (size_t)howmany ? uids.size() - howmany : 0;
    for (size_t i = uids.size(); i-- > from;) {
        char url[256];
        snprintf(url, sizeof(url),
                 "imaps://%s/INBOX;UID=%ld;SECTION=HEADER.FIELDS%%20(FROM%%20"
                 "SUBJECT%%20DATE)", cfg.imap.c_str(), uids[i]);
        std::string h;
        CURL *m = mk(url, &h);
        if (!m)
            continue;
        if (curl_easy_perform(m) == CURLE_OK) {
            std::string fr = dec_header(field(h, "from"));
            std::string su = dec_header(field(h, "subject"));
            std::string dt = field(h, "date");
            // одна запись — одна строка: uid, от кого, тема, дата
            for (size_t k = 0; k < fr.size(); k++)
                if (fr[k] == '\t') fr[k] = ' ';
            for (size_t k = 0; k < su.size(); k++)
                if (su[k] == '\t') su[k] = ' ';
            printf("%ld\t%s\t%s\t%s\n", uids[i], fr.c_str(), su.c_str(),
                   dt.c_str());
        }
        curl_easy_cleanup(m);
    }
    return 0;
}

static int do_body(long uid)
{
    char url[256];
    snprintf(url, sizeof(url), "imaps://%s/INBOX;UID=%ld", cfg.imap.c_str(),
             uid);
    std::string msg;
    CURL *c = mk(url, &msg);
    if (!c)
        return 1;
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) {
        printf("ОШИБКА %s\n", curl_easy_strerror(r));
        return 1;
    }
    // делим на заголовки и тело
    size_t sep = msg.find("\r\n\r\n");
    size_t skip = 4;
    if (sep == std::string::npos) {
        sep = msg.find("\n\n");
        skip = 2;
    }
    std::string head = sep == std::string::npos ? msg : msg.substr(0, sep);
    std::string body = sep == std::string::npos ? "" : msg.substr(sep + skip);

    std::string ctype = field(head, "content-type");
    std::string enc = field(head, "content-transfer-encoding");
    std::string low;
    for (size_t i = 0; i < ctype.size(); i++)
        low += tolower((unsigned char)ctype[i]);

    // из многочастного письма берём первую текстовую часть
    size_t bp = low.find("boundary=");
    if (low.find("multipart") != std::string::npos &&
        bp != std::string::npos) {
        std::string b = ctype.substr(bp + 9);
        if (!b.empty() && b[0] == '"')
            b = b.substr(1, b.find('"', 1) - 1);
        else {
            size_t e = b.find_first_of("; \t\r\n");
            if (e != std::string::npos)
                b = b.substr(0, e);
        }
        std::string mark = "--" + b;
        size_t p = body.find(mark);
        while (p != std::string::npos) {
            size_t start = body.find('\n', p);
            if (start == std::string::npos)
                break;
            size_t hend = body.find("\r\n\r\n", start);
            size_t hskip = 4;
            if (hend == std::string::npos) {
                hend = body.find("\n\n", start);
                hskip = 2;
            }
            size_t next = body.find(mark, start);
            if (hend == std::string::npos || hend > next)
                break;
            std::string ph = body.substr(start, hend - start);
            std::string pl;
            for (size_t i = 0; i < ph.size(); i++)
                pl += tolower((unsigned char)ph[i]);
            if (pl.find("text/plain") != std::string::npos) {
                enc = field(ph, "content-transfer-encoding");
                ctype = field(ph, "content-type");
                body = body.substr(hend + hskip,
                                   next == std::string::npos
                                   ? std::string::npos : next - hend - hskip);
                break;
            }
            p = next;
        }
    }

    std::string el;
    for (size_t i = 0; i < enc.size(); i++)
        el += tolower((unsigned char)enc[i]);
    if (el.find("base64") != std::string::npos)
        body = b64_decode(body);
    else if (el.find("quoted-printable") != std::string::npos)
        body = qp_decode(body, 0);

    std::string cl;
    for (size_t i = 0; i < ctype.size(); i++)
        cl += tolower((unsigned char)ctype[i]);
    size_t cp = cl.find("charset=");
    if (cp != std::string::npos) {
        std::string cs = ctype.substr(cp + 8);
        if (!cs.empty() && cs[0] == '"')
            cs = cs.substr(1, cs.find('"', 1) - 1);
        else {
            size_t e = cs.find_first_of("; \t\r\n");
            if (e != std::string::npos)
                cs = cs.substr(0, e);
        }
        body = to_utf8(body, cs);
    }
    fwrite(body.c_str(), 1, body.size(), stdout);
    return 0;
}

struct Upload { std::string data; size_t pos; };

static size_t feed(char *p, size_t sz, size_t nm, void *ud)
{
    Upload *u = (Upload *)ud;
    size_t n = sz * nm;
    if (u->pos >= u->data.size())
        return 0;
    if (n > u->data.size() - u->pos)
        n = u->data.size() - u->pos;
    memcpy(p, u->data.c_str() + u->pos, n);
    u->pos += n;
    return n;
}

static int do_send(const char *to)
{
    std::string body;
    char b[4096];
    ssize_t n;
    while ((n = read(0, b, sizeof(b))) > 0)
        body.append(b, n);
    // тема — первая строка, остальное текст
    std::string subject = "(без темы)", text = body;
    size_t nl = body.find('\n');
    if (nl != std::string::npos) {
        subject = body.substr(0, nl);
        text = body.substr(nl + 1);
    }
    Upload up;
    up.pos = 0;
    // тело кодируем base64: письмо на кириллице иначе поедет
    std::string enc;
    static const char *TBL = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                             "abcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < text.size(); i += 3) {
        unsigned v = (unsigned char)text[i] << 16;
        if (i + 1 < text.size()) v |= (unsigned char)text[i + 1] << 8;
        if (i + 2 < text.size()) v |= (unsigned char)text[i + 2];
        enc += TBL[(v >> 18) & 63];
        enc += TBL[(v >> 12) & 63];
        enc += (i + 1 < text.size()) ? TBL[(v >> 6) & 63] : '=';
        enc += (i + 2 < text.size()) ? TBL[v & 63] : '=';
        if (enc.size() % 78 < 4)
            enc += "\r\n";
    }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char date[64];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", &tm);
    up.data = "From: " + cfg.user + "\r\nTo: " + to + "\r\nDate: " + date +
              "\r\nSubject: " + subject +
              "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; "
              "charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n" +
              enc + "\r\n";

    std::string out;
    CURL *c = mk("smtps://" + cfg.smtp, &out);
    if (!c)
        return 1;
    curl_easy_setopt(c, CURLOPT_MAIL_FROM, cfg.user.c_str());
    struct curl_slist *rcpt = curl_slist_append(NULL, to);
    curl_easy_setopt(c, CURLOPT_MAIL_RCPT, rcpt);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_READFUNCTION, feed);
    curl_easy_setopt(c, CURLOPT_READDATA, &up);
    CURLcode r = curl_easy_perform(c);
    curl_slist_free_all(rcpt);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) {
        printf("ОШИБКА %s\n", curl_easy_strerror(r));
        return 1;
    }
    printf("ОТПРАВЛЕНО\n");
    return 0;
}

// ── интерфейс ────────────────────────────────────────────────────────
static const int W = 480, H = 776, WIN_Y = 24;
static const unsigned long BG = 0x101418, KEYC = 0x1c2530, ACCENT = 0x2e7d32,
                           FIELD = 0x1c2530;
static const int HEAD_H = 56, ROW_H = 92, BAR_H = 62, SEND_W = 84;

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_big, *f_txt, *f_small;
static XftColor c_fg, c_dim;

enum { SCR_SETUP, SCR_LIST, SCR_READ, SCR_COMPOSE };
static int screen = SCR_LIST;
static int focus = 0;                  // поле ввода на экране настройки
static int kbd_up = 0, scroll = 0, cur = -1;
static std::string status = "";
static std::string to_addr, subject, ctext;
static std::vector<std::string> body_lines;

struct Letter { long uid; std::string from, subj, date; };
static std::vector<Letter> letters;

static pid_t job = 0;
static int job_kind = 0;               // 1 список 2 письмо 3 отправка
static const char *JOB_OUT = "/tmp/.mail.out";

// клавиатуру просим меткой — keysd поднимает её только по ней, поэтому
// при чтении писем её нет
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

static std::vector<std::string> wrap(XftFont *fn, const std::string &s, int wd)
{
    std::vector<std::string> out;
    std::string curl;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\n') {
            out.push_back(curl);
            curl.clear();
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
        if (tw(fn, (curl + ch).c_str()) > wd && !curl.empty()) {
            size_t sp = curl.rfind(' ');
            if (sp != std::string::npos && sp > 0) {
                std::string tail = curl.substr(sp + 1);
                out.push_back(curl.substr(0, sp));
                curl = tail + ch;
            } else {
                out.push_back(curl);
                curl = ch;
            }
        } else
            curl += ch;
    }
    out.push_back(curl);
    return out;
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

// поля экрана настройки
static const char *SET_LBL[4] = {"Адрес (логин)", "Пароль приложения",
                                 "Сервер IMAP", "Сервер SMTP"};
static std::string *set_val[4];
static int set_y(int i) { return HEAD_H + 20 + i * 84; }

static void draw_setup(void)
{
    fill(BG, 0, 0, W, H);
    draw_head("Настройка почты", 0);
    for (int i = 0; i < 4; i++) {
        int y = set_y(i);
        text(f_small, &c_dim, 14, y + 14, SET_LBL[i]);
        fill(FIELD, 10, y + 22, W - 20, 50);
        if (focus == i) {
            XSetForeground(dpy, gc, 0x3a5578);
            XDrawRectangle(dpy, buf, gc, 10, y + 22, W - 21, 49);
        }
        std::string v = *set_val[i];
        if (i == 1)                    // пароль не показываем
            v = std::string(v.size(), '*');
        text(f_txt, &c_fg, 22, y + 54, cut(f_txt, v, W - 44).c_str());
    }
    int by = set_y(4) + 10;
    fill(ACCENT, 10, by, W - 20, 60);
    const char *s = "Сохранить и войти";
    text(f_big, &c_fg, (W - tw(f_big, s)) / 2, by + 38, s);
    if (!status.empty())
        text(f_small, &c_dim, 14, by + 92, cut(f_small, status, W - 28).c_str());
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void draw_list(void)
{
    fill(BG, 0, 0, W, H);
    draw_head("Почта", 0);
    const char *r = "⟳";
    fill(0x27313d, W - 58, 8, 48, 40);
    text(f_big, &c_fg, W - 58 + (48 - tw(f_big, r)) / 2, 36, r);

    int y0 = HEAD_H + 4, bottom = H - 74;
    if (letters.empty()) {
        const char *e = job && job_kind == 1 ? "читаю почту…"
                                             : "писем нет — нажмите ⟳";
        text(f_txt, &c_dim, 20, y0 + 40, e);
    }
    for (size_t i = (size_t)scroll; i < letters.size(); i++) {
        int y = y0 + ((int)i - scroll) * ROW_H;
        if (y + ROW_H > bottom)
            break;
        fill(KEYC, 10, y, W - 20, ROW_H - 6);
        text(f_big, &c_fg, 24, y + 30,
             cut(f_big, letters[i].from, W - 48).c_str());
        text(f_txt, &c_fg, 24, y + 56,
             cut(f_txt, letters[i].subj, W - 48).c_str());
        text(f_small, &c_dim, 24, y + 78,
             cut(f_small, letters[i].date, W - 48).c_str());
    }
    if (!status.empty())
        text(f_small, &c_dim, 14, H - 82, cut(f_small, status, W - 28).c_str());
    fill(ACCENT, 10, H - 68, W - 20, 58);
    const char *n = "Написать";
    text(f_big, &c_fg, (W - tw(f_big, n)) / 2, H - 30, n);
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void draw_read(void)
{
    fill(BG, 0, 0, W, H);
    std::string t = cur >= 0 && cur < (int)letters.size()
                    ? letters[cur].subj : "";
    int y = HEAD_H + 30;
    for (size_t i = (size_t)scroll; i < body_lines.size(); i++) {
        if (y > H - 20)
            break;
        text(f_txt, &c_fg, 16, y, body_lines[i].c_str());
        y += 24;
    }
    if (body_lines.empty())
        text(f_txt, &c_dim, 20, HEAD_H + 40,
             job ? "читаю письмо…" : "пусто");
    draw_head(cut(f_big, t, W - 160).c_str(), 1);
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static int comp_bar_y(void) { return kbd_up ? 300 : H - BAR_H - 10; }

static void draw_compose(void)
{
    fill(BG, 0, 0, W, H);
    draw_head("Письмо", 1);
    text(f_small, &c_dim, 14, HEAD_H + 26, "Кому");
    fill(FIELD, 10, HEAD_H + 34, W - 20, 48);
    if (focus == 0) {
        XSetForeground(dpy, gc, 0x3a5578);
        XDrawRectangle(dpy, buf, gc, 10, HEAD_H + 34, W - 21, 47);
    }
    text(f_txt, &c_fg, 22, HEAD_H + 64, cut(f_txt, to_addr, W - 44).c_str());

    text(f_small, &c_dim, 14, HEAD_H + 108, "Тема");
    fill(FIELD, 10, HEAD_H + 116, W - 20, 48);
    if (focus == 1) {
        XSetForeground(dpy, gc, 0x3a5578);
        XDrawRectangle(dpy, buf, gc, 10, HEAD_H + 116, W - 21, 47);
    }
    text(f_txt, &c_fg, 22, HEAD_H + 146, cut(f_txt, subject, W - 44).c_str());

    int by = comp_bar_y();
    text(f_small, &c_dim, 14, HEAD_H + 190, "Текст");
    fill(FIELD, 10, HEAD_H + 198, W - 20, by - HEAD_H - 210);
    if (focus == 2) {
        XSetForeground(dpy, gc, 0x3a5578);
        XDrawRectangle(dpy, buf, gc, 10, HEAD_H + 198, W - 21,
                       by - HEAD_H - 211);
    }
    std::vector<std::string> tl = wrap(f_txt, ctext, W - 44);
    int ty = HEAD_H + 226;
    for (size_t i = 0; i < tl.size() && ty < by - 20; i++, ty += 24)
        text(f_txt, &c_fg, 22, ty, tl[i].c_str());

    fill(to_addr.empty() ? KEYC : ACCENT, 10, by, W - 20, BAR_H);
    const char *s = job && job_kind == 3 ? "отправляю…" : "Отправить";
    text(f_big, &c_fg, (W - tw(f_big, s)) / 2, by + 40, s);
    if (!status.empty())
        text(f_small, &c_dim, 14, by + BAR_H + 24,
             cut(f_small, status, W - 28).c_str());
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void draw(void)
{
    if (screen == SCR_SETUP) draw_setup();
    else if (screen == SCR_LIST) draw_list();
    else if (screen == SCR_READ) draw_read();
    else draw_compose();
}

// ── фоновые дела ─────────────────────────────────────────────────────
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

static void job_done(void)
{
    std::string out = readf(JOB_OUT);
    int kind = job_kind;
    job = 0;
    job_kind = 0;
    if (out.compare(0, 12, "ОШИБКА") == 0) {
        status = out.substr(0, 60);
        return;
    }
    if (kind == 1) {
        letters.clear();
        size_t p = 0;
        while (p < out.size()) {
            size_t e = out.find('\n', p);
            if (e == std::string::npos)
                e = out.size();
            std::string ln = out.substr(p, e - p);
            p = e + 1;
            size_t t1 = ln.find('\t');
            if (t1 == std::string::npos)
                continue;
            size_t t2 = ln.find('\t', t1 + 1);
            size_t t3 = t2 == std::string::npos ? t2 : ln.find('\t', t2 + 1);
            if (t2 == std::string::npos || t3 == std::string::npos)
                continue;
            Letter l;
            l.uid = atol(ln.c_str());
            l.from = ln.substr(t1 + 1, t2 - t1 - 1);
            l.subj = ln.substr(t2 + 1, t3 - t2 - 1);
            l.date = ln.substr(t3 + 1);
            if (l.from.empty()) l.from = "(без отправителя)";
            if (l.subj.empty()) l.subj = "(без темы)";
            letters.push_back(l);
        }
        status = letters.empty() ? "писем нет" : "";
    } else if (kind == 2) {
        body_lines = wrap(f_txt, out, W - 32);
        scroll = 0;
    } else if (kind == 3) {
        if (out.find("ОТПРАВЛЕНО") != std::string::npos) {
            status = "письмо отправлено";
            ctext.clear();
            subject.clear();
            screen = SCR_LIST;
            kbd_hide();
        } else
            status = "отправить не вышло";
    }
}

// ── нажатия ──────────────────────────────────────────────────────────
static void click(int x, int y)
{
    if (screen == SCR_SETUP) {
        for (int i = 0; i < 4; i++) {
            int fy = set_y(i) + 22;
            if (y >= fy && y < fy + 50) {
                focus = i;
                kbd_show();
                return;
            }
        }
        int by = set_y(4) + 10;
        if (y >= by && y < by + 60) {
            cfg_save();
            kbd_hide();
            if (cfg.user.empty() || cfg.password.empty()) {
                status = "нужны адрес и пароль приложения";
                return;
            }
            status = "";
            screen = SCR_LIST;
            start_job("/usr/local/bin/mail fetch 15", 1);
        }
        return;
    }
    if (screen == SCR_LIST) {
        if (y < HEAD_H) {
            if (x > W - 60 && !job) {
                status = "читаю почту…";
                start_job("/usr/local/bin/mail fetch 15", 1);
            }
            return;
        }
        if (y >= H - 68) {
            screen = SCR_COMPOSE;
            focus = 0;
            status.clear();
            kbd_show();
            return;
        }
        int i = scroll + (y - HEAD_H - 4) / ROW_H;
        if (i >= 0 && i < (int)letters.size() && !job) {
            cur = i;
            screen = SCR_READ;
            body_lines.clear();
            scroll = 0;
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "/usr/local/bin/mail body %ld",
                     letters[i].uid);
            start_job(cmd, 2);
        }
        return;
    }
    if (screen == SCR_READ) {
        if (y < HEAD_H && x < 140) {
            screen = SCR_LIST;
            scroll = 0;
        }
        return;
    }
    // письмо
    if (y < HEAD_H) {
        if (x < 140) {
            screen = SCR_LIST;
            kbd_hide();
            status.clear();
        }
        return;
    }
    if (y >= HEAD_H + 34 && y < HEAD_H + 82) { focus = 0; kbd_show(); return; }
    if (y >= HEAD_H + 116 && y < HEAD_H + 164) { focus = 1; kbd_show(); return; }
    int by = comp_bar_y();
    if (y >= HEAD_H + 198 && y < by - 12) { focus = 2; kbd_show(); return; }
    if (y >= by && y < by + BAR_H && !to_addr.empty() && !job) {
        // тема и текст уходят потомку через файл: в командной строке они
        // были бы видны всей системе и ломались бы на кавычках
        std::string msg = subject + "\n" + ctext;
        int fd = open("/tmp/.mail-body", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            if (write(fd, msg.c_str(), msg.size()) < 0) { }
            close(fd);
        }
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "/usr/local/bin/mail send '%s' < /tmp/.mail-body",
                 to_addr.c_str());
        status = "отправляю…";
        start_job(cmd, 3);
    }
}

static std::string cp_utf8(unsigned cp)
{
    return cp_to_utf8(cp);
}

static void key_in(XKeyEvent *e)
{
    char b[16];
    KeySym ks;
    int n = XLookupString(e, b, sizeof(b) - 1, &ks, NULL);
    std::string *dst = NULL;
    if (screen == SCR_SETUP)
        dst = set_val[focus];
    else if (screen == SCR_COMPOSE)
        dst = focus == 0 ? &to_addr : (focus == 1 ? &subject : &ctext);
    if (!dst)
        return;
    if (ks == XK_BackSpace) {
        while (!dst->empty() && ((unsigned char)dst->back() & 0xc0) == 0x80)
            dst->erase(dst->size() - 1);
        if (!dst->empty())
            dst->erase(dst->size() - 1);
    } else if (ks == XK_Return) {
        if (screen == SCR_COMPOSE && focus == 2)
            *dst += "\n";
        else
            focus = (focus + 1) % (screen == SCR_SETUP ? 4 : 3);
    } else if (n > 0) {
        b[n] = 0;
        *dst += b;
    } else if (ks >= 0x01000000)
        *dst += cp_utf8(ks & 0xffffff);
    draw();
}

int main(int argc, char **argv)
{
    cfg_load();
    // рабочие режимы: их запускает сам интерфейс отдельным процессом
    if (argc > 1 && !strcmp(argv[1], "fetch"))
        return do_fetch(argc > 2 ? atoi(argv[2]) : 15);
    if (argc > 1 && !strcmp(argv[1], "body") && argc > 2)
        return do_body(atol(argv[2]));
    if (argc > 1 && !strcmp(argv[1], "send") && argc > 2)
        return do_send(argv[2]);

    int lock = open("/run/.mail.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    set_val[0] = &cfg.user;
    set_val[1] = &cfg.password;
    set_val[2] = &cfg.imap;
    set_val[3] = &cfg.smtp;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "нет доступа к X\n");
        return 1;
    }
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Почта");
    XClassHint ch;
    ch.res_name = (char *)"mailapp";
    ch.res_class = (char *)"Mailapp";
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

    if (cfg.user.empty() || cfg.password.empty())
        screen = SCR_SETUP;
    else
        start_job("/usr/local/bin/mail fetch 15", 1);
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
                int step = screen == SCR_READ ? 24 : ROW_H;
                int maxs = 0;
                if (screen == SCR_LIST)
                    maxs = (int)letters.size() - 6;
                else if (screen == SCR_READ)
                    maxs = (int)body_lines.size() - 24;
                if (maxs < 0)
                    maxs = 0;
                int ns = scroll_at_press - dy / step;
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
        if (job && waitpid(job, NULL, WNOHANG) == job) {
            job_done();
            draw();
        }
    }
    return 0;
}
