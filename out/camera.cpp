// Камера HTC HD2 — нативная замена интерфейса camera-app.
//
// Живое превью держит campreview (непрерывный поток VFE с возвратом
// буферов — единственный стабильный режим), кадр 324×243 лежит в
// /tmp/preview.rgb. Проявку превью считаем здесь, а сам снимок делает
// camshot: там остался прежний конвейер на numpy/PIL (дебайер,
// выравнивание кадров, тональная кривая) — переписывать эту математику
// заново значило бы ломать то, что работает.
//
// Камера строго однопользовательская: перед снимком поток превью гасим.
//
// Сборка: g++ -O2 camera.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o camera

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <fcntl.h>
#include <math.h>
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

static const int W = 480, H = 776, WIN_Y = 24;
static const unsigned long BG = 0x101828, KEY = 0x1c2a40, ACC = 0x1f7a33,
                           WARN = 0x8a5a2e, VID = 0x2c5aa4;

static const int PV_X = 8, PV_Y = 6, PV_W = 464, PV_H = 348;
static const int ST_Y = 380;
static const int R1_Y = 396, ROW_H = 70, R2_Y = 472;
static const int BAR_Y = 560, BAR_H = 140;

static const int PW = 324, PH = 243;         // кадр от campreview
static const int PV_LEN = PW * PH * 3;

static const char *MODES[3] = {"Авто", "Ночь", "HDR"};
static const char *FLASH[3] = {"Авто", "Вкл", "Выкл"};
static const char *FILTERS[5] = {"Без фильтра", "Ч/Б", "Сепия", "Яркий",
                                 "Мягкий"};
static const int TIMERS[3] = {0, 3, 10};

static Display *dpy;
static int scr;
static Window win;
static Pixmap pixbuf;
static GC gc;
static XftDraw *xd;
static XftFont *f_btn, *f_small, *f_shoot;
static XftColor c_fg, c_dim;

static int mode = 0, flash = 0, filt = 0, timer_i = 0;
static std::string status = "готова";
static unsigned char *rgba;                  // PV_W*PV_H*4
static XImage *img;
static int have_frame = 0;
static time_t pv_mtime = 0;

static pid_t pv_pid = 0, job = 0;
static int job_video = 0;
static int countdown = 0;                    // секунды таймера до снимка
static time_t countdown_at = 0;
static double pv_hold = 0;                   // до этого времени превью не рисуем
static const char *JOB_OUT = "/tmp/.camera.out";

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void torch(int on)
{
    int fd = open("/sys/class/leds/flashlight/brightness", O_WRONLY | O_TRUNC);
    if (fd < 0)
        return;
    if (write(fd, on ? "255" : "0", on ? 3 : 1) < 0) { }
    close(fd);
}

// ── поток превью ─────────────────────────────────────────────────────
static void pv_start(void)
{
    if (pv_pid > 0 && kill(pv_pid, 0) == 0)
        return;
    pid_t p = fork();
    if (p == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 1); dup2(null, 2); }
        execl("/usr/local/bin/campreview", "campreview", "3600", "120",
              "900", "0", "0", (char *)NULL);
        _exit(127);
    }
    pv_pid = p;
}

static void pv_stop(void)
{
    if (pv_pid <= 0)
        return;
    kill(pv_pid, SIGTERM);                   // штатная остановка потока VFE
    for (int i = 0; i < 50; i++) {
        if (waitpid(pv_pid, NULL, WNOHANG) == pv_pid)
            break;
        usleep(100000);
    }
    kill(pv_pid, SIGKILL);
    waitpid(pv_pid, NULL, WNOHANG);
    pv_pid = 0;
    usleep(300000);                          // драйверу — отпустить устройство
}

// проявка превью: вычет чёрного, «серый мир», растяжка, гамма
static void tone_preview(const unsigned char *f)
{
    long hist[256];
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < PV_LEN; i++)
        hist[f[i]]++;
    long total = PV_LEN, acc = 0;
    int black = 0, hi = 255;
    for (int v = 0; v < 256; v++) {          // 0.5-й процентиль
        acc += hist[v];
        if (acc >= total / 200) { black = v; break; }
    }
    acc = 0;
    for (int v = 255; v >= 0; v--) {         // 99.5-й процентиль
        acc += hist[v];
        if (acc >= total / 200) { hi = v; break; }
    }
    double span = hi - black;
    if (span < 8)
        span = 8;
    double sum[3] = {0, 0, 0};
    for (int i = 0; i < PV_LEN; i += 3)
        for (int c = 0; c < 3; c++) {
            double v = f[i + c] - black;
            sum[c] += v > 0 ? v : 0;
        }
    double mean = (sum[0] + sum[1] + sum[2]) / 3;
    double k[3];
    for (int c = 0; c < 3; c++)
        k[c] = mean / (sum[c] > 1 ? sum[c] : 1);

    static unsigned char lut[256];
    static int lut_span = -1;
    if (lut_span != (int)span) {             // гамма — таблицей, кадров много
        lut_span = (int)span;
        for (int v = 0; v < 256; v++) {
            double x = v / span;
            if (x > 1) x = 1;
            lut[v] = (unsigned char)(pow(x, 1.0 / 2.2) * 255);
        }
    }
    // растягиваем 324×243 до 464×348 ближайшим соседом
    for (int y = 0; y < PV_H; y++) {
        int sy = y * PH / PV_H;
        for (int x = 0; x < PV_W; x++) {
            int sx = x * PW / PV_W;
            const unsigned char *p = f + (sy * PW + sx) * 3;
            unsigned char *o = rgba + (y * PV_W + x) * 4;
            for (int c = 0; c < 3; c++) {
                double v = (p[c] - black) * k[c];
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                o[2 - c] = lut[(int)v];      // BGRA
            }
            o[3] = 0xff;
        }
    }
    have_frame = 1;
}

static void pv_read(void)
{
    struct stat st;
    if (stat("/tmp/preview.rgb", &st) < 0 || st.st_size != PV_LEN ||
        st.st_mtime == pv_mtime)
        return;
    pv_mtime = st.st_mtime;
    int fd = open("/tmp/preview.rgb", O_RDONLY);
    if (fd < 0)
        return;
    static unsigned char frame[PV_LEN];
    ssize_t got = 0, n;
    while (got < PV_LEN && (n = read(fd, frame + got, PV_LEN - got)) > 0)
        got += n;
    close(fd);
    if (got == PV_LEN)
        tone_preview(frame);
}

// показать в окне готовый снимок (PPM от camshot)
static void show_ppm(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    char magic[3] = {0};
    int w = 0, h = 0, maxv = 0;
    if (fscanf(f, "%2s %d %d %d", magic, &w, &h, &maxv) != 4 ||
        strcmp(magic, "P6") || w <= 0 || h <= 0) {
        fclose(f);
        return;
    }
    fgetc(f);                                 // один пробельный байт
    memset(rgba, 0, (size_t)PV_W * PV_H * 4);
    unsigned char *row = (unsigned char *)malloc((size_t)w * 3);
    if (!row) { fclose(f); return; }
    int ox = (PV_W - w) / 2, oy = (PV_H - h) / 2;
    for (int y = 0; y < h; y++) {
        if (fread(row, 1, (size_t)w * 3, f) != (size_t)w * 3)
            break;
        int dy = oy + y;
        if (dy < 0 || dy >= PV_H)
            continue;
        for (int x = 0; x < w; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= PV_W)
                continue;
            unsigned char *o = rgba + (dy * PV_W + dx) * 4;
            o[0] = row[x * 3 + 2];
            o[1] = row[x * 3 + 1];
            o[2] = row[x * 3];
            o[3] = 0xff;
        }
    }
    free(row);
    fclose(f);
    have_frame = 1;
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

// кнопка с подписью в две строки
static void button2(int x, int y, int w, int h, unsigned long col,
                    const char *top, const char *bottom)
{
    XSetForeground(dpy, gc, col);
    XFillRectangle(dpy, pixbuf, gc, x, y, w, h);
    if (bottom) {
        text(f_small, &c_dim, x + (w - tw(f_small, top)) / 2, y + 28, top);
        text(f_btn, &c_fg, x + (w - tw(f_btn, bottom)) / 2, y + 54, bottom);
    } else
        text(f_btn, &c_fg, x + (w - tw(f_btn, top)) / 2, y + h / 2 + 7, top);
}

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, pixbuf, gc, 0, 0, W, H);
    XSetForeground(dpy, gc, 0x000000);
    XFillRectangle(dpy, pixbuf, gc, PV_X, PV_Y, PV_W, PV_H);
    if (have_frame)
        XPutImage(dpy, pixbuf, gc, img, 0, 0, PV_X, PV_Y, PV_W, PV_H);
    else {
        const char *t = "HTC HD2 · 5 Мп";
        text(f_btn, &c_dim, PV_X + (PV_W - tw(f_btn, t)) / 2, PV_Y + PV_H / 2,
             t);
    }
    text(f_small, &c_dim, (W - tw(f_small, status.c_str())) / 2, ST_Y,
         status.c_str());

    int hw = (W - 22) / 2;
    button2(8, R1_Y, hw, ROW_H, KEY, "Режим:", MODES[mode]);
    button2(14 + hw, R1_Y, hw, ROW_H, KEY, "Вспышка:", FLASH[flash]);
    button2(8, R2_Y, hw, ROW_H, KEY, "Фильтр:", FILTERS[filt]);
    char tb[32];
    snprintf(tb, sizeof(tb), "%d с", TIMERS[timer_i]);
    button2(14 + hw, R2_Y, hw, ROW_H, KEY, "Таймер:", tb);

    int shooting = (job && !job_video) || countdown > 0;
    int recording = (job && job_video);
    int sw = 300;
    XSetForeground(dpy, gc, shooting ? WARN : ACC);
    XFillRectangle(dpy, pixbuf, gc, 12, BAR_Y, sw, BAR_H);
    const char *sl = countdown > 0 ? "таймер…" : (shooting ? "съёмка…"
                                                           : "Снимок");
    text(f_shoot, &c_fg, 12 + (sw - tw(f_shoot, sl)) / 2, BAR_Y + BAR_H / 2 + 9,
         sl);
    int vx = 12 + sw + 8, vw = W - vx - 12;
    XSetForeground(dpy, gc, recording ? WARN : VID);
    XFillRectangle(dpy, pixbuf, gc, vx, BAR_Y, vw, BAR_H);
    const char *v1 = recording ? "запись" : "Видео";
    const char *v2 = recording ? "идёт" : "10 с";
    text(f_btn, &c_fg, vx + (vw - tw(f_btn, v1)) / 2, BAR_Y + BAR_H / 2 - 6,
         v1);
    text(f_btn, &c_fg, vx + (vw - tw(f_btn, v2)) / 2, BAR_Y + BAR_H / 2 + 26,
         v2);

    XCopyArea(dpy, pixbuf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

// ── съёмка ───────────────────────────────────────────────────────────
static void start_job(const char *cmd, int video)
{
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
    job_video = video;
}

static void shoot_now(void)
{
    pv_stop();                                // камера однопользовательская
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/usr/local/bin/camshot '%s' '%s' '%s'",
             MODES[mode], FLASH[flash], FILTERS[filt]);
    status = "съёмка…";
    start_job(cmd, 0);
}

// последняя непустая строка вывода — она и есть итог
static std::string last_line(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return "";
    std::string s;
    char b[4096];
    ssize_t n;
    while ((n = read(fd, b, sizeof(b))) > 0)
        s.append(b, n);
    close(fd);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    size_t p = s.rfind('\n');
    return p == std::string::npos ? s : s.substr(p + 1);
}

static void click(int x, int y)
{
    int hw = (W - 22) / 2;
    if (y >= R1_Y && y < R1_Y + ROW_H) {
        if (x < 8 + hw) mode = (mode + 1) % 3;
        else flash = (flash + 1) % 3;
        return;
    }
    if (y >= R2_Y && y < R2_Y + ROW_H) {
        if (x < 8 + hw) filt = (filt + 1) % 5;
        else timer_i = (timer_i + 1) % 3;
        return;
    }
    if (y >= BAR_Y && y < BAR_Y + BAR_H) {
        if (job || countdown > 0)
            return;
        if (x < 12 + 300) {
            if (TIMERS[timer_i] > 0) {
                countdown = TIMERS[timer_i];
                countdown_at = time(NULL) + 1;
                char b[48];
                snprintf(b, sizeof(b), "таймер: %d…", countdown);
                status = b;
            } else
                shoot_now();
        } else {
            pv_stop();
            status = "видео: съёмка 10 с, потом сборка (~40 с)…";
            start_job("/usr/local/bin/campreview 11 120 900 10 0; "
                      "/usr/local/bin/camvid 10", 1);
        }
    }
}

int main(void)
{
    int lock = open("/tmp/.camera.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "Камера");
    XClassHint ch;
    ch.res_name = (char *)"camera";
    ch.res_class = (char *)"Camera";
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
    pixbuf = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, pixbuf, 0, NULL);
    xd = XftDrawCreate(dpy, pixbuf, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));
    f_btn = XftFontOpenName(dpy, scr, "DejaVu Sans:size=13:bold");
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=10");
    f_shoot = XftFontOpenName(dpy, scr, "DejaVu Sans:size=17:bold");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xffff, 0xffff, 0xffff, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);

    rgba = (unsigned char *)calloc((size_t)PV_W * PV_H, 4);
    if (!rgba)
        return 1;
    img = XCreateImage(dpy, vis, 24, ZPixmap, 0, (char *)rgba, PV_W, PV_H,
                       32, PV_W * 4);
    pv_start();

    int xfd = ConnectionNumber(dpy);
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose)
                draw();
            else if (e.type == ClientMessage &&
                     (Atom)e.xclient.data.l[0] == wm_del) {
                torch(0);                     // закрываем — фонарь погасить
                pv_stop();
                return 0;
            } else if (e.type == ButtonPress) {
                click(e.xbutton.x, e.xbutton.y);
                draw();
            }
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {0, 150000};
        select(xfd + 1, &fds, NULL, NULL, &tv);

        if (countdown > 0 && time(NULL) >= countdown_at) {
            countdown_at = time(NULL) + 1;
            if (--countdown <= 0) {
                countdown = 0;
                shoot_now();
            } else {
                char b[48];
                snprintf(b, sizeof(b), "таймер: %d…", countdown);
                status = b;
            }
            draw();
        }
        if (job && waitpid(job, NULL, WNOHANG) == job) {
            std::string res = last_line(JOB_OUT);
            if (res.find("СОХРАНЕНО") == 0) {   // в UTF-8 это 18 байт, не 9
                status = "сохранено: " + res.substr(res.rfind('/') + 1);
                show_ppm("/tmp/camshot-preview.ppm");
                pv_hold = now_s() + 3;        // три секунды показываем снимок
            } else
                status = res.empty() ? "готово" : res.substr(0, 60);
            job = 0;
            job_video = 0;
            pv_start();
            draw();
        }
        if (!job && countdown == 0 && now_s() >= pv_hold) {
            pv_read();
            draw();
        }
    }
    return 0;
}
