// IceMobile: домашний экран HTC HD2 — плитки в духе Windows Phone.
//
// Нативная замена прежнего home на Python/Tk: тот же вид, но ~1 МБ памяти
// вместо 17 и мгновенный запуск. Рисуем сами через Xlib, текст — Xft
// (нужен для кириллицы), двойная буферизация, чтобы не мигало.
//
// Сборка: g++ -O2 home.cpp -lX11 -lXft -lfontconfig -o home
//         (заголовки Xft лежат в /usr/include/freetype2)

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

static const int WIN_W = 480, WIN_H = 776, WIN_Y = 24;
static const int TILE_H = 118, PAD = 5, COLS = 2;
static const int HEAD_H = 96;          // часы и дата
static const unsigned long BG = 0x000000;

struct Tile {
    const char *sym;
    const char *name;
    unsigned long color;
    const char *cmd;
};

// Значки — только те, что есть в DejaVu Sans: своего запасного шрифта у
// Xft нет, а отсутствующий знак рисуется пустым квадратом.
static const Tile TILES[] = {
    {"☎", "Телефон",     0x1f7a33, "/usr/local/bin/phone-gui"},
    {"✉", "СМС",         0x0a6ebd, "/usr/local/bin/phone-sms"},
    {"◉", "Камера",      0x8a2b5a, "/usr/local/bin/camera"},
    {"♫", "Медиа",       0x7a4b8a, "/usr/local/bin/media"},
    {"▦", "Приложения",  0x2e6a5a, "/usr/local/bin/taskmgr"},
    {"=",      "Калькулятор", 0x8a5a2e, "/usr/local/bin/calc"},
    {"✎", "Заметки",     0x4a6b2e, "/usr/local/bin/notes"},
    {"◴", "Часы",        0x2c5aa4, "/usr/local/bin/clock"},
    {"@",      "Почта",       0xa4712c, "/usr/local/bin/mail"},
    {"▧", "Галерея",     0x2e6a8a, "/usr/local/bin/gallery"},
    {"☄", "Фонарик",     0x8a8a2e, "/usr/local/bin/torch"},
    {"▣", "2048",        0x6a2e8a, "/usr/local/bin/game2048"},
    {"№", "Календарь",   0xa4262c, "/usr/local/bin/calendar"},
    {"BT", "Bluetooth",   0x2a4a8a, "/usr/local/bin/btapp"},
    {"≋", "Радио",       0x8a4a2a, "/usr/local/bin/radio-app"},
    {"⚙", "Настройки",   0x4b5a6a, "/usr/local/bin/settings"},
    {"ℹ", "Система",     0x33415c, "/usr/local/bin/sysinfo"},
    {">_",     "Терминал",    0x4a4a56,
     "/usr/local/bin/fitwin Terminal /usr/local/bin/term-max"},
    {"▤", "Файлы",       0x6b5b2e, "/usr/local/bin/fitwin Xfe xfe"},
    {"⌨", "Клава",       0x5133b8, "/usr/local/bin/kbd"},
    {"◐", "Браузер",     0x7a2b4b, "/usr/local/bin/fitwin Dillo dillo"},
    {"○", "Питание",     0x5d3a3a, "/usr/local/bin/powermenu"},
    {"↻", "Графика",     0x5d5d3a, "/usr/local/bin/x-restart"},
};
static const int NTILES = sizeof(TILES) / sizeof(TILES[0]);

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_clock, *f_date, *f_sym, *f_name;
static XftColor c_fg, c_dim;

static int scroll = 0, content_h = 0;
static int press_y = 0, press_tile = -1, moved = 0;
static int flash_tile = -1;
static time_t flash_until = 0;

static void buzz(int ms)
{
    int fd = open("/sys/class/timed_output/vibrator/enable", O_WRONLY);
    if (fd < 0)
        return;
    char b[8];
    int n = snprintf(b, sizeof(b), "%d", ms);
    if (write(fd, b, n) < 0) { /* всё равно продолжаем */ }
    close(fd);
}

// Запуск приложения: отвязываем от нас полностью, чтобы наш выход его не
// уронил, и не оставляем зомби.
static void run_cmd(const char *cmd)
{
    pid_t p = fork();
    if (p != 0)
        return;
    setsid();
    if (fork() != 0)
        _exit(0);                      // промежуточный уходит, внук — сирота
    char line[256];
    snprintf(line, sizeof(line), "%s", cmd);
    char *argv[16];
    int n = 0;
    for (char *t = strtok(line, " "); t && n < 15; t = strtok(NULL, " "))
        argv[n++] = t;
    argv[n] = NULL;
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
    }
    execv(argv[0], argv);
    _exit(127);
}

static unsigned long lighten(unsigned long c)
{
    int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    r = r + 70 > 255 ? 255 : r + 70;
    g = g + 70 > 255 ? 255 : g + 70;
    b = b + 70 > 255 ? 255 : b + 70;
    return (r << 16) | (g << 8) | b;
}

static void text(XftFont *fn, XftColor *col, int x, int y, const char *s)
{
    XftDrawStringUtf8(xd, col, fn, x, y, (const FcChar8 *)s, strlen(s));
}

static int text_w(XftFont *fn, const char *s)
{
    XGlyphInfo gi;
    XftTextExtentsUtf8(dpy, fn, (const FcChar8 *)s, strlen(s), &gi);
    return gi.xOff;
}

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, WIN_W, WIN_H);

    // часы и дата
    char hm[16], ds[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(hm, sizeof(hm), "%H:%M", &tm);
    static const char *WD[] = {"вс", "пн", "вт", "ср", "чт", "пт", "сб"};
    char dm[16];
    strftime(dm, sizeof(dm), "%d.%m.%Y", &tm);
    snprintf(ds, sizeof(ds), "%s · %s", WD[tm.tm_wday], dm);
    text(f_clock, &c_fg, (WIN_W - text_w(f_clock, hm)) / 2, 62, hm);
    text(f_date, &c_dim, (WIN_W - text_w(f_date, ds)) / 2, 86, ds);

    // плитки
    int tw = (WIN_W - PAD * (COLS + 1)) / COLS;
    for (int i = 0; i < NTILES; i++) {
        int col = i % COLS, row = i / COLS;
        int x = PAD + col * (tw + PAD);
        int y = HEAD_H + PAD + row * (TILE_H + PAD) - scroll;
        if (y > WIN_H || y + TILE_H < HEAD_H)
            continue;                  // за экраном — не рисуем
        unsigned long c = TILES[i].color;
        if (i == flash_tile)
            c = lighten(c);
        XSetForeground(dpy, gc, c);
        XFillRectangle(dpy, buf, gc, x, y, tw, TILE_H);
        text(f_sym, &c_fg, x + tw - 12 - text_w(f_sym, TILES[i].sym),
             y + 32, TILES[i].sym);
        text(f_name, &c_fg, x + 12, y + TILE_H - 14, TILES[i].name);
    }
    // шапку рисуем поверх плиток, чтобы они «уезжали» под часы
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, WIN_W, HEAD_H);
    text(f_clock, &c_fg, (WIN_W - text_w(f_clock, hm)) / 2, 62, hm);
    text(f_date, &c_dim, (WIN_W - text_w(f_date, ds)) / 2, 86, ds);

    XCopyArea(dpy, buf, win, gc, 0, 0, WIN_W, WIN_H, 0, 0);
    XFlush(dpy);
}

static int tile_at(int x, int y)
{
    if (y < HEAD_H)
        return -1;
    int tw = (WIN_W - PAD * (COLS + 1)) / COLS;
    for (int i = 0; i < NTILES; i++) {
        int col = i % COLS, row = i / COLS;
        int tx = PAD + col * (tw + PAD);
        int ty = HEAD_H + PAD + row * (TILE_H + PAD) - scroll;
        if (x >= tx && x < tx + tw && y >= ty && y < ty + TILE_H)
            return i;
    }
    return -1;
}

static void clamp_scroll(void)
{
    int maxs = content_h - (WIN_H - HEAD_H);
    if (maxs < 0)
        maxs = 0;
    if (scroll > maxs)
        scroll = maxs;
    if (scroll < 0)
        scroll = 0;
}

int main(void)
{
    // один экземпляр: блокировку не удаляем — иначе взаимное исключение
    // перестаёт работать (проверено на прежней версии)
    // O_CLOEXEC обязателен: без него запущенные нами программы
    // наследуют эту блокировку и держат её после нашего выхода —
    // тогда следующий экземпляр уже не стартует (так шторка
    // держала блокировку статус-полоски)
    int lock = open("/tmp/.home.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;

    signal(SIGCHLD, SIG_IGN);          // без зомби от запущенных приложений

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "нет доступа к X\n");
        return 1;
    }
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y,
                              WIN_W, WIN_H, 0, BG, BG);
    XStoreName(dpy, win, "Домой");
    XClassHint ch;
    ch.res_name = (char *)"icehome";   // под это имя настроен IceWM
    ch.res_class = (char *)"IceHome";
    XSetClassHint(dpy, win, &ch);
    XSizeHints sh;
    sh.flags = PPosition | PSize | PMinSize | PMaxSize;
    sh.x = 0; sh.y = WIN_Y;
    sh.width = sh.min_width = sh.max_width = WIN_W;
    sh.height = sh.min_height = sh.max_height = WIN_H;
    XSetWMNormalHints(dpy, win, &sh);
    // Button1MotionMask, а НЕ PointerMotionMask: иначе движение без
    // нажатия прокручивает список, и следующий тап попадает не в ту плитку
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask |
                 ButtonReleaseMask | Button1MotionMask);
    XMapWindow(dpy, win);

    buf = XCreatePixmap(dpy, win, WIN_W, WIN_H, DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, buf, 0, NULL);
    xd = XftDrawCreate(dpy, buf, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));
    f_clock = XftFontOpenName(dpy, scr, "DejaVu Sans:size=40:bold");
    f_date = XftFontOpenName(dpy, scr, "DejaVu Sans:size=12");
    f_sym = XftFontOpenName(dpy, scr, "DejaVu Sans:size=20:bold");
    f_name = XftFontOpenName(dpy, scr, "DejaVu Sans:size=13:bold");
    if (!f_clock || !f_date || !f_sym || !f_name) {
        fprintf(stderr, "шрифт не открылся\n");
        return 1;
    }
    XRenderColor w = {0xffff, 0xffff, 0xffff, 0xffff};
    XRenderColor d = {0x9a9a, 0xa4a4, 0xb0b0, 0xffff};
    XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr), &w, &c_fg);
    XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr), &d, &c_dim);

    content_h = ((NTILES + COLS - 1) / COLS) * (TILE_H + PAD) + PAD;

    int xfd = ConnectionNumber(dpy);
    time_t last_min = 0;
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose) {
                draw();
            } else if (e.type == ButtonPress) {
                if (e.xbutton.button == 4 || e.xbutton.button == 5) {
                    scroll += (e.xbutton.button == 5 ? 60 : -60);
                    clamp_scroll();
                    draw();
                } else {
                    press_y = e.xbutton.y;
                    press_tile = tile_at(e.xbutton.x, e.xbutton.y);
                    moved = 0;
                }
            } else if (e.type == MotionNotify) {
                if (!(e.xmotion.state & Button1Mask))
                    continue;          // палец не прижат — не прокручиваем
                int dy = e.xmotion.y - press_y;
                if (dy > 12 || dy < -12) {
                    moved = 1;
                    scroll -= dy;
                    press_y = e.xmotion.y;
                    clamp_scroll();
                    draw();
                }
            } else if (e.type == ButtonRelease) {
                if (!moved && press_tile >= 0 &&
                    press_tile == tile_at(e.xbutton.x, e.xbutton.y)) {
                    buzz(35);
                    flash_tile = press_tile;
                    flash_until = time(NULL);
                    draw();
                    run_cmd(TILES[press_tile].cmd);
                }
                press_tile = -1;
            }
            // ConfigureNotify намеренно не трогаем: если возвращать окно
            // на место при каждом сообщении, начинается война с оконным
            // менеджером и экран моргает. Положение задаёт winoptions.
        }
        // ждём события X или тик времени
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {1, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);

        time_t now = time(NULL);
        if (flash_tile >= 0 && now > flash_until) {
            flash_tile = -1;
            draw();
        }
        if (now / 60 != last_min) {     // часы: перерисовка раз в минуту
            last_min = now / 60;
            draw();
        }
    }
    return 0;
}
