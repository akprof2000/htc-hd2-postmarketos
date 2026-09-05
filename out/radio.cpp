// FM-радио HTC HD2 — нативная замена radio-app на Python/Tk.
//
// Приёмник сидит в чипе BCM4329 вместе с Bluetooth; регистрами командуем
// vendor-командой HCI 0xFC15 по сырому HCI-сокету. ГЛАВНОЕ: регистр
// частоты 0x0a/0x0b — это килогерцы минус 64000 (шаг 1 кГц), а не шаги
// по 10 кГц из карты BCM2048. Звук — аналоговая петля: SWITCH_DEVICE на
// гарнитуру + AUDIO_START_FM на /dev/msm_audio_ctl.
//
// Сборка: g++ -O2 radio.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o radio-app

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

// ── HCI ──────────────────────────────────────────────────────────────
#define AF_BLUETOOTH_   31
#define BTPROTO_HCI_    1
#define HCI_FILTER_     2

struct sockaddr_hci_ {
    unsigned short family;
    unsigned short dev;
    unsigned short channel;
};

static const int W = 480, H = 752, WIN_Y = 48;
static const int BAND_LO = 87500, BAND_HI = 108000, FREQ_BASE = 64000;
static const unsigned long BG = 0x101828, FG_C = 0xe8eef5, DIM_C = 0x7c8794,
                           KEYC = 0x1c2a40, ACC = 0x1f7a33, RED = 0xa4262c;
static const char *STATIONS = "/root/.fm-stations";
static const int DEV_HEADSET = 0x107ac8a;

static int hci = -1;
static int khz = 96800, vol = 100, radio_on = 0, page = 0;
static std::vector<int> stations;
static std::string status_msg = "";
static int scanning = 0, scan_khz = 0;
static std::vector<std::pair<int, int> > scan_prof;

// ── работа с приёмником ──────────────────────────────────────────────
static int hci_open(void)
{
    int s = socket(AF_BLUETOOTH_, SOCK_RAW, BTPROTO_HCI_);
    if (s < 0)
        return -1;
    unsigned f[4] = {0xffffffff, 0xffffffff, 0xffffffff, 0};
    setsockopt(s, 0, HCI_FILTER_, f, sizeof(f));
    struct sockaddr_hci_ a;
    memset(&a, 0, sizeof(a));
    a.family = AF_BLUETOOTH_;
    a.dev = 0;
    a.channel = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    struct timeval tv = {1, 500000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return s;
}

// vendor-команда 0xFC15; ответ — параметры Command Complete
static int fm_cmd(const unsigned char *pl, int n, unsigned char *out)
{
    if (hci < 0)
        return -1;
    unsigned short op = (0x3f << 10) | 0x15;
    unsigned char pkt[64];
    pkt[0] = 0x01;
    pkt[1] = op & 0xff;
    pkt[2] = op >> 8;
    pkt[3] = n;
    memcpy(pkt + 4, pl, n);
    if (write(hci, pkt, 4 + n) < 0)
        return -1;
    for (int tries = 0; tries < 8; tries++) {
        unsigned char b[300];
        int r = read(hci, b, sizeof(b));
        if (r < 7)
            continue;
        if (b[0] == 0x04 && b[1] == 0x0e) {           // Command Complete
            unsigned short rop = b[4] | (b[5] << 8);
            if (rop != op)
                continue;
            if (b[6] != 0)
                return -1;                            // чип отверг команду
            if (out)
                memcpy(out, b + 7, r - 7);
            return r - 7;
        }
    }
    return -1;
}

static void fm_wr(int reg, int val, int size)
{
    unsigned char p[8];
    p[0] = reg;
    p[1] = 0;
    for (int i = 0; i < size; i++)
        p[2 + i] = (val >> (8 * i)) & 0xff;
    fm_cmd(p, 2 + size, NULL);
}

static int fm_rd(int reg, int size)
{
    unsigned char p[3] = {(unsigned char)reg, 1, (unsigned char)size};
    unsigned char out[64];
    int n = fm_cmd(p, 3, out);
    if (n < size)
        return -1;
    int v = 0;
    for (int i = 0; i < size; i++)
        v |= out[n - size + i] << (8 * i);
    return v;
}

static void audio_ioctl(int nr, unsigned arg)
{
    int fd = open("/dev/msm_audio_ctl", O_RDWR);
    if (fd < 0)
        return;
    unsigned code = (1u << 30) | (4u << 16) | ('a' << 8) | nr;
    ioctl(fd, code, &arg);
    close(fd);
}

static void writef(const char *path, const char *val)
{
    // O_TRUNC обязателен: без него «90» поверх «100» даёт «900»
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0)
        return;
    if (write(fd, val, strlen(val)) < 0) { }
    close(fd);
}

static std::string readf(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return "";
    char b[1024];
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0)
        return "";
    b[n] = 0;
    return std::string(b);
}

static void sh(const char *cmd)
{
    pid_t p = fork();
    if (p != 0)
        return;
    setsid();
    if (fork() != 0)
        _exit(0);
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
}

static int bt_up(void)
{
    FILE *f = popen("hciconfig hci0 2>/dev/null", "r");
    if (!f)
        return 0;
    std::string o;
    char b[256];
    while (fgets(b, sizeof(b), f))
        o += b;
    pclose(f);
    return o.find("UP RUNNING") != std::string::npos;
}

static int headset_in(void)
{
    std::string s = readf("/sys/class/switch/h2w/state");
    return s.empty() ? 0 : (atoi(s.c_str()) & 3);
}

static void fm_on_seq(void)
{
    fm_wr(0x00, 0x01, 1);
    usleep(50000);
    fm_wr(0x00, 0x01, 1);
    fm_wr(0x10, 0x0000, 2);
    fm_wr(0x01, 0x06, 1);              // Европа, стерео + авто
    fm_wr(0x14, 0x40, 1);
    fm_wr(0x05, 0x001c, 2);            // 50 мкс, DAC L+R, без mute
    fm_wr(0x4d, 0x40, 1);              // аналоговый выход
    fm_wr(0x04, 0, 1);
}

static void fm_tune(int k)
{
    fm_wr(0x0a, k - FREQ_BASE, 2);     // килогерцы, а не шаги по 10!
    fm_wr(0x09, 0x01, 1);              // preset tune
    usleep(200000);
    char b[16];
    snprintf(b, sizeof(b), "%d", k);
    writef("/run/fm", b);
}

static void save_stations(void)
{
    std::sort(stations.begin(), stations.end());
    stations.erase(std::unique(stations.begin(), stations.end()),
                   stations.end());
    std::string s;
    char b[16];
    for (size_t i = 0; i < stations.size(); i++) {
        snprintf(b, sizeof(b), "%d ", stations[i]);
        s += b;
    }
    int fd = open(STATIONS, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, s.c_str(), s.size()) < 0) { }
        close(fd);
    }
}

static void load_stations(void)
{
    stations.clear();
    std::string s = readf(STATIONS);
    const char *p = s.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\n')
            p++;
        if (!*p)
            break;
        int v = atoi(p);
        if (v > 0)
            stations.push_back(v);
        while (*p && *p != ' ' && *p != '\n')
            p++;
    }
    std::sort(stations.begin(), stations.end());
}

// ── окно ─────────────────────────────────────────────────────────────
static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_freq, *f_btn, *f_small;
static XftColor c_fg, c_dim;

static const int FREQ_Y = 96, INFO_Y = 128, SCALE_Y = 146, SCALE_H = 56;
// HINT_Y — это БАЗОВАЯ ЛИНИЯ подписи: буквы уходят вверх примерно на 12
// точек, поэтому она должна начинаться заметно ниже кнопок ряда 2,
// иначе текст ложится прямо на них.
static const int ROW1_Y = 214, ROW2_Y = 278, HINT_Y = 356;
static const int GRID_Y = 368, CELL_H = 88, VOL_Y = 564, POWER_Y = 650;
static const int POWER_H = 84;
static const int BTN_H = 54;

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

static void button(int x, int y, int w, int h, unsigned long col,
                   const char *label, XftFont *fn)
{
    XSetForeground(dpy, gc, col);
    XFillRectangle(dpy, buf, gc, x, y, w, h);
    // длинная надпись мельче, иначе упирается в края кнопки
    if (tw(fn, label) > w - 10)
        fn = f_small;
    text(fn, &c_fg, x + (w - tw(fn, label)) / 2, y + h / 2 + 7, label);
}

static int pages(void)
{
    int n = (int)stations.size();
    return n <= 0 ? 1 : (n + 5) / 6;
}

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);

    char b[64];
    snprintf(b, sizeof(b), "%.1f", khz / 1000.0);
    text(f_freq, radio_on ? &c_fg : &c_dim, (W - tw(f_freq, b)) / 2,
         FREQ_Y, b);

    // сигнал и стерео
    std::string info = "наушники работают антенной";
    if (radio_on && !scanning) {
        int lvl = fm_rd(0x0f, 1), fl = fm_rd(0x12, 1);
        if (lvl >= 0) {
            int n = lvl - 0x90;
            if (n < 0) n = 0;
            if (n > 5) n = 5;
            info = (fl > 0 && (fl & 0x40)) ? "стерео   " : "моно   ";
            for (int i = 0; i < 5; i++)
                info += (i < n) ? "#" : "-";
        }
    }
    if (!status_msg.empty())
        info = status_msg;
    text(f_small, &c_dim, (W - tw(f_small, info.c_str())) / 2, INFO_Y,
         info.c_str());

    // шкала со станциями
    XSetForeground(dpy, gc, KEYC);
    XFillRectangle(dpy, buf, gc, 10, SCALE_Y, W - 20, SCALE_H);
    for (int m = 88; m < 108; m++) {
        int x = 10 + (m * 1000 - BAND_LO) * (W - 20) / (BAND_HI - BAND_LO);
        XSetForeground(dpy, gc, DIM_C);
        XDrawLine(dpy, buf, gc, x, SCALE_Y + 34, x,
                  SCALE_Y + (m % 2 ? 48 : 56));
        if (m % 4 == 0) {
            snprintf(b, sizeof(b), "%d", m);
            text(f_small, &c_dim, x - 8, SCALE_Y + 16, b);
        }
    }
    for (size_t i = 0; i < stations.size(); i++) {
        int x = 10 + (stations[i] - BAND_LO) * (W - 20) / (BAND_HI - BAND_LO);
        XSetForeground(dpy, gc, ACC);
        XFillArc(dpy, buf, gc, x - 3, SCALE_Y + 22, 7, 7, 0, 360 * 64);
    }
    int x = 10 + (khz - BAND_LO) * (W - 20) / (BAND_HI - BAND_LO);
    XSetForeground(dpy, gc, 0xe0a030);
    XFillRectangle(dpy, buf, gc, x - 1, SCALE_Y, 3, SCALE_H);

    // ряд перестройки
    const char *nav[] = {"<<", "<", ">", ">>"};
    int bw = (W - 20 - 3 * 6) / 4;
    for (int i = 0; i < 4; i++)
        button(10 + i * (bw + 6), ROW1_Y, bw, BTN_H, KEYC, nav[i], f_btn);

    // поиск и ввод частоты
    int hw = (W - 20 - 6) / 2;
    button(10, ROW2_Y, hw, BTN_H, scanning ? RED : KEYC,
           scanning ? "идёт поиск, стоп" : "Поиск станций", f_btn);
    button(10 + hw + 6, ROW2_Y, hw, BTN_H, KEYC, "Частота", f_btn);

    snprintf(b, sizeof(b), "%d станций · %d/%d · долгий тап удаляет",
             (int)stations.size(), page + 1, pages());
    text(f_small, &c_dim, (W - tw(f_small, b)) / 2, HINT_Y, b);

    // список станций, 6 на страницу
    int cw = (W - 20 - 2 * 6) / 3;
    for (int i = 0; i < 6; i++) {
        int c = i % 3, r = i / 3;
        int cx = 10 + c * (cw + 6), cy = GRID_Y + r * (CELL_H + 6);
        size_t idx = (size_t)page * 6 + i;
        int f = idx < stations.size() ? stations[idx] : 0;
        unsigned long col = (f && f == khz) ? ACC : KEYC;
        XSetForeground(dpy, gc, col);
        XFillRectangle(dpy, buf, gc, cx, cy, cw, CELL_H);
        if (f)
            snprintf(b, sizeof(b), "%.1f", f / 1000.0);
        else
            snprintf(b, sizeof(b), "+");
        text(f_btn, f ? &c_fg : &c_dim, cx + (cw - tw(f_btn, b)) / 2,
             cy + CELL_H / 2 + 7, b);
    }

    // громкость и питание
    button(10, VOL_Y, hw, BTN_H, KEYC, "тише", f_btn);
    button(10 + hw + 6, VOL_Y, hw, BTN_H, KEYC, "громче", f_btn);
    button(10, POWER_Y, W - 20, POWER_H, radio_on ? RED : ACC,
           radio_on ? "Выключить" : "Включить", f_btn);

    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

// ── действия ─────────────────────────────────────────────────────────
static void radio_start(void)
{
    if (!headset_in()) {
        status_msg = "вставьте наушники: без них нет антенны";
        draw();
        return;
    }
    if (!bt_up()) {
        status_msg = "поднимаю чип, ~20 с…";
        draw();
        system("/usr/local/bin/btsetup >/dev/null 2>&1");
    }
    if (hci < 0)
        hci = hci_open();
    if (hci < 0) {
        status_msg = "нет доступа к чипу";
        draw();
        return;
    }
    fm_on_seq();
    audio_ioctl(32, DEV_HEADSET);
    audio_ioctl(38, 0);                // закрыть возможный старый сеанс
    audio_ioctl(37, 0);
    audio_ioctl(10, vol);
    writef("/sys/class/htc_accessory/fm/flag", "fm_headset\n");
    radio_on = 1;
    fm_tune(khz);
    status_msg = "";
    draw();
}

static void radio_stop(void)
{
    if (radio_on) {
        audio_ioctl(38, 0);
        fm_wr(0x00, 0x00, 1);
        writef("/sys/class/htc_accessory/fm/flag", "disable\n");
    }
    radio_on = 0;
    status_msg = "выключено";
    draw();
}

static void tune_to(int k)
{
    if (k < BAND_LO) k = BAND_LO;
    if (k > BAND_HI) k = BAND_HI;
    khz = (k + 50) / 100 * 100;
    if (radio_on)
        fm_tune(khz);
    draw();
}

static void hop(int d)
{
    if (stations.empty()) {
        tune_to(khz + d * 1000);
        return;
    }
    if (d > 0) {
        for (size_t i = 0; i < stations.size(); i++)
            if (stations[i] > khz + 50) { tune_to(stations[i]); return; }
        tune_to(stations.front());
    } else {
        for (int i = (int)stations.size() - 1; i >= 0; i--)
            if (stations[i] < khz - 50) { tune_to(stations[i]); return; }
        tune_to(stations.back());
    }
}

static void remember(int k)
{
    for (size_t i = 0; i < stations.size(); i++)
        if (stations[i] == k) {
            status_msg = "уже в списке";
            return;
        }
    stations.push_back(k);
    save_stations();
    status_msg = "запомнено";
}

// поиск: шагаем по диапазону, станция = уровень выше шума
static void scan_step(void)
{
    if (!scanning)
        return;
    fm_tune(scan_khz);
    int lvl = 0;
    for (int i = 0; i < 3; i++) {
        int v = fm_rd(0x0f, 1);
        if (v > lvl)
            lvl = v;
    }
    scan_prof.push_back(std::make_pair(scan_khz, lvl));
    char b[64];
    snprintf(b, sizeof(b), "поиск: %.1f МГц", scan_khz / 1000.0);
    status_msg = b;
    khz = scan_khz;
    draw();
    scan_khz += 100;
    if (scan_khz > BAND_HI) {           // конец: выбираем вершины
        int noise = 0xff;
        for (size_t i = 0; i < scan_prof.size(); i++)
            noise = std::min(noise, scan_prof[i].second);
        std::vector<std::pair<int, int> > cand;
        for (size_t i = 0; i < scan_prof.size(); i++)
            if (scan_prof[i].second >= noise + 3)
                cand.push_back(std::make_pair(scan_prof[i].second,
                                              scan_prof[i].first));
        std::sort(cand.rbegin(), cand.rend());
        std::vector<int> found;
        for (size_t i = 0; i < cand.size(); i++) {
            int ok = 1;
            for (size_t j = 0; j < found.size(); j++)
                if (abs(cand[i].second - found[j]) < 300)
                    ok = 0;
            if (ok)
                found.push_back(cand[i].second);
        }
        for (size_t i = 0; i < found.size(); i++)
            stations.push_back(found[i]);
        save_stations();
        scanning = 0;
        page = 0;
        snprintf(b, sizeof(b), "найдено станций: %d", (int)found.size());
        status_msg = b;
        if (!stations.empty())
            tune_to(stations[0]);
        draw();
    }
}

// ── ввод частоты цифрами ─────────────────────────────────────────────
static int keypad_mode = 0;
static std::string keypad_buf;

static void draw_keypad(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);
    std::string s = keypad_buf.empty() ? "---" : keypad_buf;
    text(f_freq, &c_fg, (W - tw(f_freq, s.c_str())) / 2, 110, s.c_str());
    const char *hint = "например 101.7";
    text(f_small, &c_dim, (W - tw(f_small, hint)) / 2, 150, hint);
    static const char *K[] = {"1", "2", "3", "4", "5", "6",
                              "7", "8", "9", ".", "0", "<"};
    int bw = (W - 40 - 2 * 8) / 3, bh = 74;
    for (int i = 0; i < 12; i++) {
        int c = i % 3, r = i / 3;
        button(20 + c * (bw + 8), 180 + r * (bh + 8), bw, bh, KEYC, K[i],
               f_btn);
    }
    button(20, 180 + 4 * (bh + 8) + 10, (W - 48) / 2, 64, KEYC, "Отмена",
           f_btn);
    button(28 + (W - 48) / 2, 180 + 4 * (bh + 8) + 10, (W - 48) / 2, 64,
           ACC, "Настроить", f_btn);
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

static void keypad_click(int x, int y)
{
    static const char *K[] = {"1", "2", "3", "4", "5", "6",
                              "7", "8", "9", ".", "0", "<"};
    int bw = (W - 40 - 2 * 8) / 3, bh = 74;
    for (int i = 0; i < 12; i++) {
        int c = i % 3, r = i / 3;
        int bx = 20 + c * (bw + 8), by = 180 + r * (bh + 8);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            if (!strcmp(K[i], "<")) {
                if (!keypad_buf.empty())
                    keypad_buf.erase(keypad_buf.size() - 1);
            } else if (keypad_buf.size() < 5) {
                keypad_buf += K[i];
            }
            draw_keypad();
            return;
        }
    }
    int by = 180 + 4 * (bh + 8) + 10;
    if (y >= by && y < by + 64) {
        if (x > W / 2) {                       // «Настроить»
            double f = atof(keypad_buf.c_str());
            if (f > 80 && f < 110)
                tune_to((int)(f * 1000 + 0.5));
        }
        keypad_mode = 0;
        keypad_buf.clear();
        draw();
    }
}

// ── нажатия в основном окне ──────────────────────────────────────────
static time_t press_time = 0;
static int press_cell = -1;

static void click(int x, int y, int press)
{
    int bw = (W - 20 - 3 * 6) / 4, hw = (W - 20 - 6) / 2;
    int cw = (W - 20 - 2 * 6) / 3;

    if (press) {                               // запоминаем для долгого тапа
        press_cell = -1;
        for (int i = 0; i < 6; i++) {
            int c = i % 3, r = i / 3;
            int cx = 10 + c * (cw + 6), cy = GRID_Y + r * (CELL_H + 6);
            if (x >= cx && x < cx + cw && y >= cy && y < cy + CELL_H) {
                press_cell = i;
                press_time = time(NULL);
            }
        }
        return;
    }

    if (y >= SCALE_Y && y < SCALE_Y + SCALE_H) {
        tune_to(BAND_LO + (x - 10) * (BAND_HI - BAND_LO) / (W - 20));
        return;
    }
    if (y >= ROW1_Y && y < ROW1_Y + BTN_H) {
        int i = (x - 10) / (bw + 6);
        if (i == 0) hop(-1);
        else if (i == 1) tune_to(khz - 100);
        else if (i == 2) tune_to(khz + 100);
        else hop(+1);
        return;
    }
    if (y >= ROW2_Y && y < ROW2_Y + BTN_H) {
        if (x < 10 + hw) {
            if (!radio_on)
                return;
            if (scanning) {
                scanning = 0;
                status_msg = "поиск остановлен";
            } else {
                scanning = 1;
                scan_khz = BAND_LO;
                scan_prof.clear();
            }
        } else {
            keypad_mode = 1;
            keypad_buf.clear();
            draw_keypad();
            return;
        }
        draw();
        return;
    }
    for (int i = 0; i < 6; i++) {
        int c = i % 3, r = i / 3;
        int cx = 10 + c * (cw + 6), cy = GRID_Y + r * (CELL_H + 6);
        if (x >= cx && x < cx + cw && y >= cy && y < cy + CELL_H) {
            size_t idx = (size_t)page * 6 + i;
            int held = (press_cell == i) && (time(NULL) - press_time >= 1);
            if (idx < stations.size()) {
                if (held) {                    // долгий тап — удалить
                    char b[64];
                    snprintf(b, sizeof(b), "удалено %.1f",
                             stations[idx] / 1000.0);
                    status_msg = b;
                    stations.erase(stations.begin() + idx);
                    save_stations();
                    if (page >= pages())
                        page = pages() - 1;
                } else {
                    tune_to(stations[idx]);
                }
            } else {
                remember(khz);
            }
            draw();
            return;
        }
    }
    if (y >= HINT_Y - 22 && y < HINT_Y + 6) {  // подпись листает страницы
        page = (page + 1) % pages();
        draw();
        return;
    }
    if (y >= VOL_Y && y < VOL_Y + BTN_H) {
        vol += (x < 10 + hw) ? -15 : +15;
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        audio_ioctl(10, vol);
        char b[32];
        snprintf(b, sizeof(b), "громкость %d%%", vol);
        status_msg = b;
        draw();
        return;
    }
    if (y >= POWER_Y) {
        if (radio_on)
            radio_stop();
        else
            radio_start();
        return;
    }
}

int main(void)
{
    // O_CLOEXEC обязателен: без него запущенные нами программы
    // наследуют эту блокировку и держат её после нашего выхода —
    // тогда следующий экземпляр уже не стартует (так шторка
    // держала блокировку статус-полоски)
    int lock = open("/run/.radio.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);

    load_stations();
    std::string f = readf("/run/fm");
    if (!f.empty() && atoi(f.c_str()) > 0)
        khz = atoi(f.c_str());

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "нет доступа к X\n");
        return 1;
    }
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Радио");
    XClassHint ch;
    ch.res_name = (char *)"radio";
    ch.res_class = (char *)"Radio";
    XSetClassHint(dpy, win, &ch);
    XSizeHints sh_;
    sh_.flags = PPosition | PSize | PMinSize | PMaxSize;
    sh_.x = 0; sh_.y = WIN_Y;
    sh_.width = sh_.min_width = sh_.max_width = W;
    sh_.height = sh_.min_height = sh_.max_height = H;
    XSetWMNormalHints(dpy, win, &sh_);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask |
                 ButtonReleaseMask);
    // без этого «крестик» в полоске (WM_DELETE_WINDOW) окно не закрывает
    Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);
    XMapWindow(dpy, win);

    buf = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, buf, 0, NULL);
    xd = XftDrawCreate(dpy, buf, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));
    f_freq = XftFontOpenName(dpy, scr, "DejaVu Sans:size=50:bold");
    f_btn = XftFontOpenName(dpy, scr, "DejaVu Sans:size=14:bold");
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11");
    XRenderColor wc = {0xe8e8, 0xeeee, 0xf5f5, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr), &wc, &c_fg);
    XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr), &dc, &c_dim);

    int xfd = ConnectionNumber(dpy);
    int started = 0;
    time_t last_info = 0;
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose) {
                keypad_mode ? draw_keypad() : draw();
            } else if (e.type == ButtonPress) {
                if (!keypad_mode)
                    click(e.xbutton.x, e.xbutton.y, 1);
            } else if (e.type == ClientMessage) {
                if ((Atom)e.xclient.data.l[0] == wm_del) {
                    radio_stop();          // гасим приёмник и выходим
                    return 0;
                }
            } else if (e.type == ButtonRelease) {
                if (keypad_mode)
                    keypad_click(e.xbutton.x, e.xbutton.y);
                else
                    click(e.xbutton.x, e.xbutton.y, 0);
            }
        }
        if (!started) {                        // включаемся сразу после показа
            started = 1;
            radio_start();
        }
        if (scanning) {
            scan_step();
            continue;                          // поиск идёт без пауз
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {2, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);
        time_t now = time(NULL);
        if (!keypad_mode && radio_on && now - last_info >= 2) {
            last_info = now;                   // обновляем уровень сигнала
            draw();
        }
    }
    return 0;
}
