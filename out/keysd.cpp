// Аппаратные кнопки HTC HD2 — нативная замена прежнего keysd на Python.
//
// Зелёная/красная/домик/Windows/назад, качелька громкости, подсветка
// кнопок, автосон с захватом тача и автопоказ клавиатуры. Всё в одном
// цикле ожидания: два устройства ввода и таймеры, без потоков.
//
// Сборка: g++ -O2 keysd.cpp -o keysd

#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>

static const char *KEYPAD = "/dev/input/event4";
static const char *TOUCH = "/dev/input/event0";
static const char *BL = "/sys/class/leds/lcd-backlight/brightness";
static const char *BTN_BL = "/sys/class/leds/button-backlight/brightness";
static const char *VOLFILE = "/run/phone/vol";

enum {
    K_SEND = 231, K_HOME = 102, K_MENU = 139, K_END = 107, K_BACK = 158,
    K_VOLDOWN = 114, K_VOLUP = 115
};
static const double LONG_PRESS = 2.5;      // красная: выключение питания
static const double SLEEP_AFTER = 120.0;   // автосон
static const double BTN_LIGHT = 3.0;

// окна, где нужен ввод текста — там клавиатура показывается сама
static const char *TYPING[] = {"xfce4-terminal", "phone-sms", "sms",
                               "notes", "dillo", "mailapp", NULL};

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static std::string readf(const char *path)
{
    int fd = open(path, O_RDONLY);
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

static void writef(const char *path, const char *val)
{
    // O_TRUNC обязателен: без него «90» поверх «100» даёт «900».
    // O_CREAT тоже: после перезагрузки /run/phone/vol ещё нет, и без него
    // громкость не сохранялась — каждое нажатие считалось от 70.
    int fd = open(path, O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (fd < 0)
        return;
    if (write(fd, val, strlen(val)) < 0) { /* не критично */ }
    close(fd);
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

static std::string capture(const char *cmd)
{
    FILE *f = popen(cmd, "r");
    if (!f)
        return "";
    std::string out;
    char b[256];
    while (fgets(b, sizeof(b), f))
        out += b;
    pclose(f);
    return out;
}

static std::string call_state(void) { std::string s = readf("/run/phone/state"); return s.empty() ? "idle" : s; }

static void at_cmd(const char *c)
{
    int fd = open("/run/phone/cmd", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return;
    std::string s = std::string(c) + "\n";
    if (write(fd, s.c_str(), s.size()) < 0) { }
    close(fd);
}

// ── громкость ────────────────────────────────────────────────────────
static int fm_active(void)
{
    return readf("/sys/class/htc_accessory/fm/flag") == "fm_headset";
}

static void set_hw_volume(int vol)
{
    int fd = open("/dev/msm_audio_ctl", O_RDWR);
    if (fd < 0)
        return;
    unsigned code = (1u << 30) | (4u << 16) | ('a' << 8) | 10;
    unsigned v = vol < 0 ? 0 : (vol > 100 ? 100 : vol);
    ioctl(fd, code, &v);
    close(fd);
}

static void vol_osd(int vol)
{
    int fd = open("/run/shade.fifo", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return;
    char b[32];
    int n = snprintf(b, sizeof(b), "vol %d\n", vol);
    if (write(fd, b, n) < 0) { }
    close(fd);
}

// Разгон качельки: одно нажатие — шаг VOL_STEP, каждое следующее подряд
// (или повтор при удержании) увеличивает шаг на VOL_ACCEL до VOL_MAX;
// пауза дольше VOL_GAP сбрасывает разгон обратно.
static const int VOL_STEP = 1, VOL_ACCEL = 2, VOL_MAX = 12;
static const double VOL_GAP = 0.7;
static double vol_last_time = 0;
static int vol_cur_step = VOL_STEP;

static int volume_next_step(void)
{
    double t = now_s();
    if (t - vol_last_time > VOL_GAP)
        vol_cur_step = VOL_STEP;
    else
        vol_cur_step = (vol_cur_step + VOL_ACCEL > VOL_MAX)
                       ? VOL_MAX : vol_cur_step + VOL_ACCEL;
    vol_last_time = t;
    return vol_cur_step;
}

static int beep_allowed = 1;

static void volume_step(int delta)
{
    std::string cur = readf(VOLFILE);
    int v = cur.empty() ? 70 : atoi(cur.c_str());
    v += delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    char b[8];
    snprintf(b, sizeof(b), "%d", v);
    writef(VOLFILE, b);
    vol_osd(v);
    if (fm_active()) {
        // при работающем радио блик в динамик переключил бы выход с
        // наушников и порвал аналоговую петлю FM
        set_hw_volume(v);
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "@vol %d", v);
    at_cmd(cmd);
    // Звук подтверждения: строго ОДИН за раз, и только если звуковое
    // устройство свободно. Признак занятости — блокировка /run/audio.lock:
    // её берёт наш потомок и держит до самой смерти (после exec файловый
    // дескриптор остаётся у него). Если предыдущий блик завис в драйвере
    // намертво (состояние D, SIGKILL не берёт), блокировка не отпустится и
    // новых мы уже не создадим — раньше их накапливалось по два десятка и
    // звук заклинивало до перезагрузки.
    static double last_beep = 0;
    double t = now_s();
    if (!beep_allowed || call_state() != "idle" || v <= 0 ||
        t - last_beep < 0.5)
        return;
    int alock = open("/run/audio.lock", O_CREAT | O_RDWR, 0644);
    if (alock < 0)
        return;
    if (flock(alock, LOCK_EX | LOCK_NB) < 0) {
        close(alock);                  // звук ещё занят — блик пропускаем
        return;
    }
    last_beep = t;
    snprintf(cmd, sizeof(cmd), "/usr/local/bin/beep s %d", v);
    pid_t p = fork();
    if (p == 0) {
        setsid();
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(alock);                      // блокировку дальше держит потомок
}

// ── экран ────────────────────────────────────────────────────────────
static void screen_toggle(void)
{
    std::string cur = readf(BL);
    writef(BL, (!cur.empty() && atoi(cur.c_str()) > 0) ? "0" : "180");
}

// есть ли на экране окно, в которое печатают
static int any_typing_window(void)
{
    std::string list = capture("DISPLAY=:0 wmctrl -lx 2>/dev/null");
    for (char &c : list)
        c = tolower((unsigned char)c);
    for (int i = 0; TYPING[i]; i++)
        if (list.find(TYPING[i]) != std::string::npos)
            return 1;
    return 0;
}

int main(void)
{
    // O_CLOEXEC обязателен: без него запущенные нами программы
    // наследуют эту блокировку и держат её после нашего выхода —
    // тогда следующий экземпляр уже не стартует (так шторка
    // держала блокировку статус-полоски)
    int lock = open("/tmp/.keysd.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return 0;                      // два сторожа мигают клавиатурой
    // Занятость звука сторожит блокировка, а не waitpid, поэтому потомков
    // пусть подбирает ядро — иначе копятся зомби.
    signal(SIGCHLD, SIG_IGN);

    int kfd = open(KEYPAD, O_RDONLY);
    if (kfd < 0) {
        fprintf(stderr, "нет %s\n", KEYPAD);
        return 1;
    }
    // эксклюзивный захват: иначе кнопки видит ещё и X
    ioctl(kfd, EVIOCGRAB, 1);
    int tfd = open(TOUCH, O_RDONLY);   // для автосна; может и не быть

    double btn_off_at = 0, press_at[512] = {0};
    double last_touch = now_s(), last_kbd_check = 0;
    int dark = 0, hung_up = 0, kbd_want_last = -1, touched = 0;
    int screen_off_by_us = 0, woke_by_press = 0;
    double last_kbd_show = 0;

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(kfd, &fds);
        if (tfd >= 0)
            FD_SET(tfd, &fds);
        int mx = kfd > tfd ? kfd : tfd;
        struct timeval tv = {1, 0};
        select(mx + 1, &fds, NULL, NULL, &tv);

        // ── кнопки ───────────────────────────────────────────────────
        if (FD_ISSET(kfd, &fds)) {
            struct input_event ev;
            if (read(kfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) &&
                ev.type == EV_KEY) {
                if (ev.value == 1) {           // нажатие
                    last_touch = now_s();      // кнопки — тоже активность
                    woke_by_press = 0;
                    if (dark) {                // и будят экран
                        writef(BL, "180");
                        if (tfd >= 0)
                            ioctl(tfd, EVIOCGRAB, 0);
                        dark = 0;
                        screen_off_by_us = 0;
                        woke_by_press = 1;     // это нажатие уже разбудило
                    }
                    writef(BTN_BL, "255");
                    btn_off_at = now_s() + BTN_LIGHT;
                    press_at[ev.code & 511] = now_s();
                    std::string st = call_state();
                    switch (ev.code) {
                    case K_VOLUP:   volume_step(+volume_next_step()); break;
                    case K_VOLDOWN: volume_step(-volume_next_step()); break;
                    case K_SEND:
                        if (st == "ringing")
                            at_cmd("ATA");
                        else
                            sh("DISPLAY=:0 wmctrl -a 'Телефон' || "
                               "DISPLAY=:0 /usr/local/bin/phone-gui");
                        break;
                    case K_END:
                        if (st != "idle") {
                            at_cmd("ATH");
                            hung_up = 1;
                        }
                        break;
                    case K_HOME:
                        sh("DISPLAY=:0 wmctrl -a 'Домой'");
                        break;
                    case K_MENU:
                        sh("DISPLAY=:0 /usr/local/bin/sysmenu");
                        break;
                    case K_BACK:
                        sh("DISPLAY=:0 sh -c '"
                           "n=$(xdotool getactivewindow getwindowname "
                           "2>/dev/null); [ \"$n\" != \"Домой\" ] && "
                           "wmctrl -c :ACTIVE:'");
                        break;
                    }
                } else if (ev.value == 2) {    // удержание: сам повторяет
                    beep_allowed = 0;          // без звука: он бы захлебнулся
                    if (ev.code == K_VOLUP)
                        volume_step(+volume_next_step());
                    else if (ev.code == K_VOLDOWN)
                        volume_step(-volume_next_step());
                    beep_allowed = 1;
                } else if (ev.value == 0) {    // отпускание
                    double d = now_s() - press_at[ev.code & 511];
                    press_at[ev.code & 511] = 0;
                    if (ev.code == K_END) {
                        if (hung_up)
                            hung_up = 0;       // этим нажатием сбросили вызов
                        else if (woke_by_press)
                            woke_by_press = 0; // этим нажатием включили экран
                        else if (d >= LONG_PRESS)
                            sh("DISPLAY=:0 /usr/local/bin/powermenu");
                        else {
                            screen_toggle();
                            screen_off_by_us = (readf(BL) == "0");
                            if (screen_off_by_us && tfd >= 0) {
                                ioctl(tfd, EVIOCGRAB, 1);
                                dark = 1;
                            }
                        }
                    }
                }
            }
        }

        // ── касания: автосон и пробуждение ───────────────────────────
        if (tfd >= 0 && FD_ISSET(tfd, &fds)) {
            struct input_event ev;
            if (read(tfd, &ev, sizeof(ev)) > 0) { }
            last_touch = now_s();
            touched = 1;                       // для показа клавиатуры
            if (dark) {                        // просыпаемся
                writef(BL, "180");
                ioctl(tfd, EVIOCGRAB, 0);
                dark = 0;
                screen_off_by_us = 0;
            }
        }

        double t = now_s();
        if (btn_off_at && t > btn_off_at) {
            writef(BTN_BL, "0");
            btn_off_at = 0;
        }
        if (tfd >= 0) {
            std::string b = readf(BL);
            int lit = !b.empty() && atoi(b.c_str()) > 0;
            if (lit && !dark && t - last_touch > SLEEP_AFTER &&
                call_state() == "idle") {
                writef(BL, "0");
                ioctl(tfd, EVIOCGRAB, 1);      // тап-пробуждение не нажимает
                dark = 1;                      // ничего в интерфейсе
                screen_off_by_us = 1;
            }
            // Сенсор перехватываем ТОЛЬКО когда гасим экран сами: иначе
            // при любом чужом изменении яркости интерфейс переставал
            // отзываться, хотя экран горел.
            if (!lit && !dark && screen_off_by_us) {
                ioctl(tfd, EVIOCGRAB, 1);
                dark = 1;
            }
        }

        // ── автопоказ клавиатуры ─────────────────────────────────────
        if (t - last_kbd_check > 1.0) {
            last_kbd_check = t;
            std::string cls = capture("DISPLAY=:0 xdotool getactivewindow "
                                      "getwindowclassname 2>/dev/null");
            for (char &c : cls)
                c = tolower((unsigned char)c);
            // Клавиатура сама стала активным окном — значит поле ввода
            // закрылось; убираем её, если окон с вводом больше нет.
            if (cls.find("rukbd") != std::string::npos &&
                !any_typing_window()) {
                kbd_want_last = 0;
                sh("pkill -f 'bin/ruk[b]d'");
            }
            // служебные окна решения не меняют, иначе клавиатура мигает
            if (cls.find("rukbd") == std::string::npos &&
                cls.find("statusbar") == std::string::npos &&
                cls.find("volosd") == std::string::npos &&
                cls.find("shade") == std::string::npos) {
                int want = 0;
                for (int i = 0; TYPING[i]; i++)
                    if (cls.find(TYPING[i]) != std::string::npos)
                        want = 1;
                // system() здесь бесполезен: при SIGCHLD=SIG_IGN он не может
                // получить код возврата и всегда отдаёт -1
                int on = !capture("pgrep -f 'bin/ruk[b]d'").empty();
                if (want != kbd_want_last) {
                    kbd_want_last = want;
                    if (want && !on)
                        sh("DISPLAY=:0 /usr/local/bin/kbd");
                    else if (!want && on)
                        sh("pkill -f 'bin/ruk[b]d'");
                } else if (want && !on && touched && t - last_kbd_show > 2) {
                    // клавиатуру закрыли вручную, а человек снова тапнул
                    // по полю ввода — показываем её опять
                    last_kbd_show = t;
                    sh("DISPLAY=:0 /usr/local/bin/kbd");
                }
                touched = 0;
            }
        }
    }
    return 0;
}
