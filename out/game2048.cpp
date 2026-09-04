// 2048 для HTC HD2 — нативная замена game2048 на Python/Tk.
//
// Свайпы пальцем: направление берём из разницы между нажатием и
// отпусканием. Ниже поля — кнопка «Заново».
//
// Сборка: g++ -O2 game2048.cpp -I/usr/include/freetype2 -lX11 -lXft
//         -lfontconfig -o game2048

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static const int W = 480, H = 776, WIN_Y = 24;
static const unsigned long BG = 0x101828, BTN = 0x33415c;
static const int HEAD_Y = 44, HINT_Y = 72;
static const int GRID_X = 6, GRID_Y = 96, CELL = 114, PAD = 4;
static const int NEW_Y = 594, NEW_H = 76;

static Display *dpy;
static int scr;
static Window win;
static Pixmap buf;
static GC gc;
static XftDraw *xd;
static XftFont *f_head, *f_hint, *f_big, *f_small, *f_btn;
static XftColor c_fg, c_dim;

static int board[4][4];
static long score;
static int over = 0;                   // ходов больше нет
static int won = 0;                    // 2048 собрана

// цвет плитки по номиналу
static unsigned long tile_color(int v)
{
    switch (v) {
    case 0:    return 0x1c2a40;
    case 2:    return 0x33415c;
    case 4:    return 0x3a5a7c;
    case 8:    return 0x0a6ebd;
    case 16:   return 0x217a6b;
    case 32:   return 0x1f7a33;
    case 64:   return 0x8a5a2e;
    case 128:  return 0xa4712c;
    case 256:  return 0xa4262c;
    case 512:  return 0x7a2b4b;
    case 1024: return 0x5133b8;
    case 2048: return 0x7a4b8a;
    default:   return 0xe8b93c;
    }
}

static void spawn(void)
{
    int free_r[16], free_c[16], n = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (!board[r][c]) { free_r[n] = r; free_c[n] = c; n++; }
    if (!n)
        return;
    int i = rand() % n;
    board[free_r[i]][free_c[i]] = (rand() % 10 == 0) ? 4 : 2;
}

// ходов не осталось: поле забито и рядом нет одинаковых
static int no_moves(void)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (!board[r][c])
                return 0;
            if (c < 3 && board[r][c] == board[r][c + 1])
                return 0;
            if (r < 3 && board[r][c] == board[r + 1][c])
                return 0;
        }
    return 1;
}

static void reset(void)
{
    memset(board, 0, sizeof(board));
    score = 0;
    over = 0;
    won = 0;
    spawn();
    spawn();
}

// сдвиг одной линии к началу со слиянием пар
static void slide(int *v)
{
    int out[4] = {0, 0, 0, 0}, o = 0;
    int tmp[4], t = 0;
    for (int i = 0; i < 4; i++)
        if (v[i])
            tmp[t++] = v[i];
    for (int i = 0; i < t;) {
        if (i + 1 < t && tmp[i] == tmp[i + 1]) {
            out[o++] = tmp[i] * 2;
            score += tmp[i] * 2;
            i += 2;
        } else
            out[o++] = tmp[i++];
    }
    memcpy(v, out, sizeof(out));
}

static void move(char d)
{
    int old[4][4];
    memcpy(old, board, sizeof(board));
    for (int i = 0; i < 4; i++) {
        int line[4];
        for (int k = 0; k < 4; k++) {
            switch (d) {
            case 'l': line[k] = board[i][k]; break;
            case 'r': line[k] = board[i][3 - k]; break;
            case 'u': line[k] = board[k][i]; break;
            default:  line[k] = board[3 - k][i]; break;
            }
        }
        slide(line);
        for (int k = 0; k < 4; k++) {
            switch (d) {
            case 'l': board[i][k] = line[k]; break;
            case 'r': board[i][3 - k] = line[k]; break;
            case 'u': board[k][i] = line[k]; break;
            default:  board[3 - k][i] = line[k]; break;
            }
        }
    }
    if (memcmp(old, board, sizeof(board)))
        spawn();
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (board[r][c] >= 2048)
                won = 1;
    over = no_moves();
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

static void draw(void)
{
    XSetForeground(dpy, gc, BG);
    XFillRectangle(dpy, buf, gc, 0, 0, W, H);
    char b[64];
    snprintf(b, sizeof(b), "Счёт: %ld", score);
    text(f_head, &c_fg, (W - tw(f_head, b)) / 2, HEAD_Y, b);
    const char *h = "свайпы пальцем · цель 2048";
    text(f_hint, &c_dim, (W - tw(f_hint, h)) / 2, HINT_Y, h);

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int x = GRID_X + c * (CELL + PAD), y = GRID_Y + r * (CELL + PAD);
            XSetForeground(dpy, gc, tile_color(board[r][c]));
            XFillRectangle(dpy, buf, gc, x, y, CELL, CELL);
            if (!board[r][c])
                continue;
            snprintf(b, sizeof(b), "%d", board[r][c]);
            XftFont *fn = board[r][c] < 1000 ? f_big : f_small;
            text(fn, &c_fg, x + (CELL - tw(fn, b)) / 2, y + CELL / 2 + 12, b);
        }

    if (over || won) {
        // табличка поверх поля: без неё было непонятно, что игра кончена
        int mh = 132, my = GRID_Y + (4 * CELL + 3 * PAD - mh) / 2;
        XSetForeground(dpy, gc, over ? 0xa4262c : 0x1f7a33);
        XFillRectangle(dpy, buf, gc, 20, my, W - 40, mh);
        const char *t1 = over ? "Игра окончена" : "Собрано 2048!";
        text(f_head, &c_fg, (W - tw(f_head, t1)) / 2, my + 46, t1);
        snprintf(b, sizeof(b), "Счёт: %ld", score);
        text(f_hint, &c_fg, (W - tw(f_hint, b)) / 2, my + 76, b);
        const char *t3 = over ? "нажмите «Заново»" : "можно играть дальше";
        text(f_hint, &c_fg, (W - tw(f_hint, t3)) / 2, my + 104, t3);
    }

    XSetForeground(dpy, gc, BTN);
    XFillRectangle(dpy, buf, gc, 60, NEW_Y, W - 120, NEW_H);
    const char *n = "Заново";
    text(f_btn, &c_fg, (W - tw(f_btn, n)) / 2, NEW_Y + 48, n);
    XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

int main(void)
{
    int lock = open("/run/.2048.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;
    signal(SIGCHLD, SIG_IGN);
    srand((unsigned)time(NULL));
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, WIN_Y, W, H, 0,
                              BG, BG);
    XStoreName(dpy, win, "2048");
    XClassHint ch;
    ch.res_name = (char *)"game2048";
    ch.res_class = (char *)"Game2048";
    XSetClassHint(dpy, win, &ch);
    XSizeHints sh;
    sh.flags = PPosition | PSize | PMinSize | PMaxSize;
    sh.x = 0; sh.y = WIN_Y;
    sh.width = sh.min_width = sh.max_width = W;
    sh.height = sh.min_height = sh.max_height = H;
    XSetWMNormalHints(dpy, win, &sh);
    Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask);
    XMapWindow(dpy, win);
    buf = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, buf, 0, NULL);
    xd = XftDrawCreate(dpy, buf, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));
    f_head = XftFontOpenName(dpy, scr, "DejaVu Sans:size=18:bold");
    f_hint = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11");
    f_big = XftFontOpenName(dpy, scr, "DejaVu Sans:size=24:bold");
    f_small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=18:bold");
    f_btn = XftFontOpenName(dpy, scr, "DejaVu Sans:size=14:bold");
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    XRenderColor wc = {0xffff, 0xffff, 0xffff, 0xffff};
    XRenderColor dc = {0x7c7c, 0x8787, 0x9494, 0xffff};
    XftColorAllocValue(dpy, vis, cm, &wc, &c_fg);
    XftColorAllocValue(dpy, vis, cm, &dc, &c_dim);

    reset();
    int sx = 0, sy = 0;
    for (;;) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == Expose)
            draw();
        else if (e.type == ClientMessage &&
                 (Atom)e.xclient.data.l[0] == wm_del)
            return 0;
        else if (e.type == ButtonPress) {
            sx = e.xbutton.x;
            sy = e.xbutton.y;
        } else if (e.type == ButtonRelease) {
            int dx = e.xbutton.x - sx, dy = e.xbutton.y - sy;
            int adx = abs(dx), ady = abs(dy);
            if (adx < 30 && ady < 30) {         // это не свайп, а тап
                if (e.xbutton.y >= NEW_Y && e.xbutton.y < NEW_Y + NEW_H)
                    reset();
            } else if (!over && adx > ady)
                move(dx < 0 ? 'l' : 'r');
            else if (!over)
                move(dy < 0 ? 'u' : 'd');
            draw();
        }
    }
    return 0;
}
