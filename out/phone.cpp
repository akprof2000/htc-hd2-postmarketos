// Звонилка HTC HD2 — нативная замена phone-gui на Python/Tk.
//
// С модемом напрямую не разговаривает: состояние читает из /run/phone/,
// команды шлёт демону phoned через очередь /run/phone/cmd. Голосовым
// трактом тоже управляет демон (@route/@mute) — так канал модема и звук
// остаются у одного владельца.
//
// Сборка: g++ -O2 phone.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o phone-gui

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

static const int W = 480, H = 752, WIN_Y = 48;
static const unsigned long BG = 0x101418, FG_C = 0xe8eef5, DIM_C = 0x7c8794,
                           ACCENT = 0x2e7d32, DANGER = 0xc62828,
                           KEYC = 0x1c2530, WARN = 0xffa726;
static const char *RUN = "/run/phone";
static const char *HIST = "/var/lib/phone/history";

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_num, *f_key, *f_btn, *f_small, *f_mono;
static XftColor c_fg, c_dim, c_warn, c_ok;

static std::string typed, state = "idle";
static time_t call_start = 0;
static int muted = 0, hist_open = 0;
struct HistRow { std::string when, num, note; int missed; };
static std::vector<HistRow> hist_rows;
static std::vector<std::string> hist_numbers;

// ── система ──────────────────────────────────────────────────────────
static std::string readf(const std::string &path)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return "";
    char b[4096];
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

static std::string rd(const char *name)
{
    return readf(std::string(RUN) + "/" + name);
}

static void send_cmd(const std::string &line)
{
    int fd = open((std::string(RUN) + "/cmd").c_str(),
                  O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return;
    std::string s = line + "\n";
    if (write(fd, s.c_str(), s.size()) < 0) { }
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

// Раскладка: строка состояния, номер, поле 3x4, кнопки разговора,
// «Позвонить/Сброс», нижний ряд. Координаты фиксированные — так проще
// следить, чтобы элементы не наезжали друг на друга.
static const int STATUS_Y = 26, DISP_Y = 84;
static const int PAD_Y = 120, PAD_H = 400, PAD_GAP = 6;
static const int TOOLS_Y = 532, TOOLS_H = 56;
static const int ACT_Y = 598, ACT_H = 76;
static const int LOW_Y = 686, LOW_H = 52;

static const char *KEYS = "123456789*0#";

static void draw_history(void)
{
    fill(BG, 0, 0, W, H);
    const char *t = "Журнал вызовов";
    text(f_btn, &c_fg, (W - tw(f_btn, t)) / 2, 34, t);
    for (size_t i = 0; i < hist_rows.size() && i < 16; i++) {
        int y = 60 + (int)i * 36;
        fill(KEYC, 8, y, W - 16, 32);
        XftColor *col = hist_rows[i].missed ? &c_warn : &c_fg;
        text(f_mono, col, 16, y + 22, hist_rows[i].when.c_str());
        // номер ставим по фактической ширине времени, иначе колонки
        // наезжают друг на друга при широком шрифте
        text(f_mono, col, 215, y + 22, hist_rows[i].num.c_str());
        // итог прижимаем к правому краю, иначе длинные номера его срезают
        text(f_mono, col,
             W - 20 - tw(f_mono, hist_rows[i].note.c_str()), y + 22,
             hist_rows[i].note.c_str());
    }
    fill(KEYC, 8, H - 66, W - 16, 56);
    const char *cl = "Закрыть";
    text(f_btn, &c_fg, (W - tw(f_btn, cl)) / 2, H - 30, cl);
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void draw(void)
{
    if (hist_open) {
        draw_history();
        return;
    }
    fill(BG, 0, 0, W, H);

    // строка состояния
    const char *st_text = "Готов";
    XftColor *st_col = &c_dim;
    if (state == "ringing") { st_text = "ВХОДЯЩИЙ ВЫЗОВ"; st_col = &c_ok; }
    else if (state == "active") {
        st_text = "Разговор  ·  цифры шлют тоны"; st_col = &c_warn;
    } else if (state == "dialing") { st_text = "Набор…"; st_col = &c_warn; }
    text(f_small, st_col, 12, STATUS_Y, st_text);

    // номер или таймер разговора
    char line[128];
    if (state == "idle") {
        snprintf(line, sizeof(line), "%s",
                 typed.empty() ? "—" : typed.c_str());
    } else if (state == "active" && call_start) {
        int d = (int)(time(NULL) - call_start);
        std::string num = rd("number");
        snprintf(line, sizeof(line), "%s   %d:%02d",
                 num.empty() ? "разговор" : num.c_str(), d / 60, d % 60);
    } else {
        std::string num = rd("number");
        snprintf(line, sizeof(line), "%s",
                 num.empty() ? "неизвестный" : num.c_str());
    }
    text(f_num, &c_fg, (W - tw(f_num, line)) / 2, DISP_Y, line);

    // поле набора 3x4
    int cw = (W - 16 - 2 * PAD_GAP) / 3, rh = (PAD_H - 3 * PAD_GAP) / 4;
    for (int i = 0; i < 12; i++) {
        int c = i % 3, r = i / 3;
        int x = 8 + c * (cw + PAD_GAP), y = PAD_Y + r * (rh + PAD_GAP);
        fill(KEYC, x, y, cw, rh);
        char d[2] = {KEYS[i], 0};
        text(f_key, &c_fg, x + (cw - tw(f_key, d)) / 2, y + rh / 2 + 12, d);
    }

    // кнопки разговора — только когда есть разговор
    if (state == "active") {
        int bw = (W - 16 - PAD_GAP) / 2;
        fill(KEYC, 8, TOOLS_Y, bw, TOOLS_H);
        const char *m = muted ? "Микрофон ВЫКЛ" : "Микрофон вкл";
        text(f_small, muted ? &c_warn : &c_fg,
             8 + (bw - tw(f_small, m)) / 2, TOOLS_Y + 34, m);
        std::string route = rd("route");
        const char *r = route == "speaker" ? "Динамик"
                        : (route == "headset" ? "Гарнитура" : "Трубка");
        fill(KEYC, 8 + bw + PAD_GAP, TOOLS_Y, bw, TOOLS_H);
        text(f_small, &c_fg, 8 + bw + PAD_GAP + (bw - tw(f_small, r)) / 2,
             TOOLS_Y + 34, r);
    }

    // позвонить / сброс
    int bw = (W - 16 - PAD_GAP) / 2;
    const char *call_label = state == "ringing" ? "Ответить"
                             : (state == "idle" ? "Позвонить" : "—");
    fill(state == "idle" || state == "ringing" ? ACCENT : KEYC,
         8, ACT_Y, bw, ACT_H);
    text(f_btn, &c_fg, 8 + (bw - tw(f_btn, call_label)) / 2,
         ACT_Y + ACT_H / 2 + 8, call_label);
    fill(DANGER, 8 + bw + PAD_GAP, ACT_Y, bw, ACT_H);
    const char *hang = "Сброс";
    text(f_btn, &c_fg, 8 + bw + PAD_GAP + (bw - tw(f_btn, hang)) / 2,
         ACT_Y + ACT_H / 2 + 8, hang);

    // нижний ряд
    const char *er = "< стереть";
    const char *jr = "Журнал";
    text(f_small, &c_dim, 8 + (bw - tw(f_small, er)) / 2, LOW_Y + 32, er);
    text(f_small, &c_dim, 8 + bw + PAD_GAP + (bw - tw(f_small, jr)) / 2,
         LOW_Y + 32, jr);

    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

// ── журнал вызовов ───────────────────────────────────────────────────
static void load_history(void)
{
    hist_rows.clear();
    hist_numbers.clear();
    std::string all = readf(HIST);
    std::vector<std::string> rows;
    size_t p = 0;
    while (p < all.size()) {
        size_t e = all.find('\n', p);
        if (e == std::string::npos)
            e = all.size();
        if (e > p)
            rows.push_back(all.substr(p, e - p));
        p = e + 1;
    }
    for (int i = (int)rows.size() - 1; i >= 0 && hist_rows.size() < 40; i--) {
        // время;направление;номер;итог;длительность
        std::vector<std::string> f;
        size_t s = 0;
        while (s <= rows[i].size()) {
            size_t e = rows[i].find(';', s);
            if (e == std::string::npos)
                e = rows[i].size();
            f.push_back(rows[i].substr(s, e - s));
            s = e + 1;
        }
        if (f.size() < 5)
            continue;
        const char *mark = f[1] == "in" ? "<-" : (f[1] == "out" ? "->" : "?");
        char note[24];
        int missed = (f[3] == "missed");
        if (missed)
            snprintf(note, sizeof(note), "проп.");
        else if (f[3] == "ok") {
            int d = atoi(f[4].c_str());
            snprintf(note, sizeof(note), "%d:%02d", d / 60, d % 60);
        } else
            snprintf(note, sizeof(note), "нет");
        HistRow r;
        r.when = std::string(mark) + " " + f[0];
        // последние 10 цифр: длинные номера налезали бы на итог справа
        r.num = f[2].size() > 10 ? f[2].substr(f[2].size() - 10) : f[2];
        r.note = note;
        r.missed = missed;
        hist_rows.push_back(r);
        hist_numbers.push_back(f[2]);
    }
    if (hist_rows.empty()) {
        HistRow r; r.when = "журнал пуст"; r.missed = 0;
        hist_rows.push_back(r);
        hist_numbers.push_back("");
    }
    // журнал просмотрен — значок пропущенных гаснет
    int fd = open((std::string(RUN) + "/missed").c_str(),
                  O_WRONLY | O_TRUNC);
    if (fd >= 0) {
        if (write(fd, "0", 1) < 0) { }
        close(fd);
    }
}

// ── нажатия ──────────────────────────────────────────────────────────
static void click(int x, int y)
{
    if (hist_open) {
        if (y >= H - 66) {
            hist_open = 0;
            draw();
            return;
        }
        int i = (y - 60) / 36;
        if (i >= 0 && i < (int)hist_numbers.size() &&
            !hist_numbers[i].empty()) {
            typed = hist_numbers[i];
            hist_open = 0;
        }
        draw();
        return;
    }

    int cw = (W - 16 - 2 * PAD_GAP) / 3, rh = (PAD_H - 3 * PAD_GAP) / 4;
    if (y >= PAD_Y && y < PAD_Y + PAD_H) {
        for (int i = 0; i < 12; i++) {
            int c = i % 3, r = i / 3;
            int bx = 8 + c * (cw + PAD_GAP), by = PAD_Y + r * (rh + PAD_GAP);
            if (x >= bx && x < bx + cw && y >= by && y < by + rh) {
                if (state == "idle") {
                    typed += KEYS[i];
                } else if (state == "active") {
                    char c2[24];
                    snprintf(c2, sizeof(c2), "AT+VTS=%c", KEYS[i]);
                    send_cmd(c2);          // тоновый набор в разговоре
                }
                draw();
                return;
            }
        }
    }

    int bw = (W - 16 - PAD_GAP) / 2;
    if (state == "active" && y >= TOOLS_Y && y < TOOLS_Y + TOOLS_H) {
        if (x < 8 + bw) {
            muted = !muted;
            send_cmd(muted ? "@mute 1" : "@mute 0");
        } else {
            std::string route = rd("route");
            // трубка -> динамик -> гарнитура -> трубка
            send_cmd(route == "handset" ? "@route l"
                     : (route == "speaker" ? "@route h" : "@route s"));
        }
        draw();
        return;
    }
    if (y >= ACT_Y && y < ACT_Y + ACT_H) {
        if (x < 8 + bw) {
            if (state == "ringing")
                send_cmd("ATA");
            else if (state == "idle" && !typed.empty())
                send_cmd("ATD" + typed + ";");
        } else {
            send_cmd("ATH");
            typed.clear();
        }
        draw();
        return;
    }
    if (y >= LOW_Y && y < LOW_Y + LOW_H) {
        if (x < 8 + bw) {
            if (!typed.empty())
                typed.erase(typed.size() - 1);
        } else {
            load_history();
            hist_open = 1;
        }
        draw();
    }
}

int main(int argc, char **argv)
{
    // «phone-gui log» открывается сразу журналом: по уведомлению о
    // пропущенных вызовах нужен именно он, а не пустая клавиатура
    int start_log = (argc > 1 && !strcmp(argv[1], "log"));
    int lock = open("/run/phone/gui.lock", O_CREAT | O_RDWR | O_CLOEXEC,
                    0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;                      // вторая копия дробит экран пополам
    signal(SIGCHLD, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "нет доступа к X\n");
        return 1;
    }
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Телефон");
    XClassHint ch;
    ch.res_name = (char *)"phone";
    ch.res_class = (char *)"Phone";
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
    f_num = XftFontOpenName(dpy, scr, "DejaVu Sans:size=24:bold");
    f_key = XftFontOpenName(dpy, scr, "DejaVu Sans:size=22");
    f_btn = XftFontOpenName(dpy, scr, "DejaVu Sans:size=15:bold");
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=12");
    f_mono = XftFontOpenName(dpy, scr, "DejaVu Sans Mono:size=10");
    if (!f_mono)
        f_mono = f_small;
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XRenderColor yc = {0xffff, 0xa7a7, 0x2626, 0xffff};
    XRenderColor gc2 = {0x6666, 0xbbbb, 0x6a6a, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);
    XftColorAllocValue(dpy, vis, cm, &yc, &c_warn);
    XftColorAllocValue(dpy, vis, cm, &gc2, &c_ok);

    if (start_log) {
        load_history();
        hist_open = 1;
    }

    int xfd = ConnectionNumber(dpy);
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose)
                draw();
            else if (e.type == ButtonPress)
                click(e.xbutton.x, e.xbutton.y);
            else if (e.type == ClientMessage &&
                     (Atom)e.xclient.data.l[0] == wm_del)
                return 0;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {0, 700000};
        select(xfd + 1, &fds, NULL, NULL, &tv);

        std::string st = rd("state");
        if (st.empty())
            st = "idle";
        if (st != state) {
            if (st == "active")
                call_start = time(NULL);
            else {
                call_start = 0;
                if (muted) {           // к новому разговору — с микрофоном
                    muted = 0;
                    send_cmd("@mute 0");
                }
            }
            state = st;
        }
        draw();
    }
    return 0;
}
