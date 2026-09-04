/* phoned — демон модема HTC HD2.
 *
 * Канал /dev/smd0 может держать открытым только один процесс, поэтому
 * демон владеет им единолично: читает события модема, раскладывает
 * состояние по файлам и принимает команды из очереди. Интерфейс с
 * модемом напрямую не разговаривает — только через этот демон.
 *
 * Демон же управляет голосовым трактом: при переходе в разговор
 * запускает `voice on`, при завершении — `voice off`. Интерфейсу звук
 * не принадлежит.
 *
 * Состояние в /run/phone/:
 *     state    — idle | ringing | active | dialing
 *     number   — номер собеседника (если известен)
 *     route    — handset | headset | speaker
 *     mute     — 0 | 1
 *     log      — последние события
 *     cmd      — очередь (FIFO): AT-команды либо служебные «@route h»,
 *                «@mute 1», «@vol 80»
 * История вызовов — /var/lib/phone/history:
 *     время;направление(in/out);номер;итог(ok/missed/busy);длительность
 *
 * Сборка: gcc -O2 phoned.c -o phoned
 */
#define _GNU_SOURCE          /* timegm, strncasecmp */

#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEV      "/dev/smd0"
#define RUN      "/run/phone"
#define CMDFIFO  "/run/phone/cmd"
#define HISTDIR  "/var/lib/phone"
#define HIST     "/var/lib/phone/history"
#define AUDIOCTL "/dev/msm_audio_ctl"

/* _IOW('a', nr, unsigned) */
#define AUDIO_SET_VOLUME    _IOW('a', 10, unsigned)
#define AUDIO_SWITCH_DEVICE _IOW('a', 32, unsigned)
#define AUDIO_SET_MUTE      _IOW('a', 33, unsigned)

/* идентификаторы устройств ADSP (как в voice.c) */
#define DEV_HANDSET_SPKR 0x1081511u
#define DEV_HANDSET_MIC  0x1081512u
#define DEV_HEADSET_SPKR 0x107ac8au
#define DEV_HEADSET_MIC  0x1081510u
#define DEV_SPKR_MONO    0x1081513u

static int modem_fd = -1;

/* ── файлы состояния ────────────────────────────────────────────── */
static void write_state(const char *name, const char *val)
{
	char tmp[128], dst[128];
	snprintf(tmp, sizeof(tmp), RUN "/%s.tmp", name);
	snprintf(dst, sizeof(dst), RUN "/%s", name);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return;
	if (write(fd, val, strlen(val)) < 0) { }
	close(fd);
	rename(tmp, dst);          /* подмена целиком: читатель не увидит полработы */
}

static void read_state(const char *name, char *out, int n)
{
	char p[128];
	snprintf(p, sizeof(p), RUN "/%s", name);
	out[0] = 0;
	int fd = open(p, O_RDONLY);
	if (fd < 0)
		return;
	int r = read(fd, out, n - 1);
	close(fd);
	if (r < 0)
		r = 0;
	out[r] = 0;
	while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' '))
		out[--r] = 0;
}

static void logline(const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char stamp[16];
	strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
	int fd = open(RUN "/log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
		return;
	char line[600];
	int n = snprintf(line, sizeof(line), "%s %s\n", stamp, msg);
	if (write(fd, line, n) < 0) { }
	off_t sz = lseek(fd, 0, SEEK_END);
	close(fd);
	if (sz > 100000) {         /* подрезаем: журнал не должен расти вечно */
		FILE *f = fopen(RUN "/log", "r");
		if (!f)
			return;
		static char keep[200][300];
		int cnt = 0;
		char b[300];
		while (fgets(b, sizeof(b), f)) {
			strncpy(keep[cnt % 200], b, sizeof(keep[0]) - 1);
			keep[cnt % 200][sizeof(keep[0]) - 1] = 0;
			cnt++;
		}
		fclose(f);
		f = fopen(RUN "/log", "w");
		if (!f)
			return;
		int start = cnt > 200 ? cnt - 200 : 0;
		for (int i = start; i < cnt; i++)
			fputs(keep[i % 200], f);
		fclose(f);
	}
}

/* ── звук ───────────────────────────────────────────────────────── */
static int audio_ioctl(unsigned req, unsigned a, unsigned b, int nwords)
{
	int fd = open(AUDIOCTL, O_RDWR);
	if (fd < 0) {
		logline("звук: нет %s", AUDIOCTL);
		return 0;
	}
	unsigned words[2] = {a, b};
	int r = ioctl(fd, req, nwords == 1 ? (void *)&words[0] : (void *)words);
	close(fd);
	if (r < 0)
		logline("звук: ioctl %x не прошёл", req);
	return r == 0;
}

static void run_wait(const char *a0, const char *a1, const char *a2)
{
	pid_t p = fork();
	if (p == 0) {
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) { dup2(null, 1); dup2(null, 2); }
		execl(a0, a0, a1, a2, (char *)NULL);
		_exit(127);
	}
	if (p > 0) {
		int st;
		waitpid(p, &st, 0);
	}
}

static int voice_on = 0;

static void audio_route(void)
{
	if (!voice_on)
		return;
	char route[32];
	read_state("route", route, sizeof(route));
	unsigned rx = DEV_HANDSET_SPKR, tx = DEV_HANDSET_MIC;
	if (!strcmp(route, "headset")) {
		rx = DEV_HEADSET_SPKR;
		tx = DEV_HEADSET_MIC;
	} else if (!strcmp(route, "speaker")) {
		rx = DEV_SPKR_MONO;
		tx = DEV_HANDSET_MIC;
	}
	audio_ioctl(AUDIO_SWITCH_DEVICE, rx, 0, 2);
	audio_ioctl(AUDIO_SWITCH_DEVICE, tx, 0, 2);
	logline("маршрут: %s", route[0] ? route : "handset");
}

static void audio_mute(void)
{
	if (!voice_on)
		return;
	char m[8];
	read_state("mute", m, sizeof(m));
	audio_ioctl(AUDIO_SET_MUTE, !strcmp(m, "1") ? 1 : 0, 0, 1);
}

static void audio_start(void)
{
	if (voice_on)
		return;
	char route[32];
	read_state("route", route, sizeof(route));
	run_wait("/usr/local/bin/voice", "on",
		 !strcmp(route, "headset") ? "h" : NULL);
	voice_on = 1;
	audio_route();
	audio_mute();
	logline("голос: включён");
}

static void audio_stop(void)
{
	if (!voice_on)
		return;
	run_wait("/usr/local/bin/voice", "off", NULL);
	voice_on = 0;
	logline("голос: выключен");
}

/* ── звонок ─────────────────────────────────────────────────────── */
static pid_t ring_pid = 0;
static double ring_last = 0;

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void ringer_tick(int ringing)
{
	if (!ringing)
		return;
	double t = now_s();
	if (t - ring_last < 5.0)
		return;
	if (ring_pid > 0 && waitpid(ring_pid, NULL, WNOHANG) == 0)
		return;                    /* мелодия ещё играет */
	ring_last = t;
	pid_t p = fork();
	if (p == 0) {
		setsid();
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) { dup2(null, 1); dup2(null, 2); }
		execl("/usr/local/bin/ringtone", "ringtone", (char *)NULL);
		_exit(127);
	}
	ring_pid = p;
}

/* ── состояние вызова ───────────────────────────────────────────── */
static char call_state[16] = "idle";
static char call_number[64] = "";
static char call_dir[8] = "";
static double call_started = 0;
static int call_answered = 0;

static void call_finish(void)
{
	int dur = call_answered ? (int)(time(NULL) - call_started) : 0;
	const char *outcome;
	if (call_answered)
		outcome = "ok";
	else if (!strcmp(call_dir, "in")) {
		outcome = "missed";
		char m[16];
		read_state("missed", m, sizeof(m));
		char b[16];
		snprintf(b, sizeof(b), "%d", atoi(m) + 1);
		write_state("missed", b);
	} else
		outcome = "busy";
	if (call_number[0] || call_dir[0]) {
		FILE *f = fopen(HIST, "a");
		if (f) {
			time_t now = time(NULL);
			struct tm tm;
			localtime_r(&now, &tm);
			char stamp[32];
			strftime(stamp, sizeof(stamp), "%d.%m %H:%M", &tm);
			fprintf(f, "%s;%s;%s;%s;%d\n", stamp,
				call_dir[0] ? call_dir : "?",
				call_number[0] ? call_number : "?", outcome, dur);
			fclose(f);
		}
	}
	call_number[0] = 0;
	call_dir[0] = 0;
	write_state("number", "");
}

static void set_state(const char *st)
{
	if (!strcmp(st, call_state))
		return;
	char old[16];
	snprintf(old, sizeof(old), "%s", call_state);
	snprintf(call_state, sizeof(call_state), "%s", st);
	write_state("state", st);
	logline("состояние: %s -> %s", old, st);

	if (!strcmp(st, "ringing") && !strcmp(old, "idle")) {
		snprintf(call_dir, sizeof(call_dir), "in");
		call_answered = 0;
	}
	if (!strcmp(st, "dialing") && !strcmp(old, "idle")) {
		snprintf(call_dir, sizeof(call_dir), "out");
		call_answered = 0;
	}
	/* голос включаем уже на наборе: гудки «вызов идёт» сеть отдаёт по
	 * голосовому каналу ещё до ответа абонента */
	if (!strcmp(st, "dialing") && !strcmp(call_dir, "out"))
		audio_start();
	if (!strcmp(st, "active")) {
		call_answered = 1;
		call_started = time(NULL);
		audio_start();
	} else if (!strcmp(st, "idle"))
		audio_stop();

	if (!strcmp(st, "idle") && strcmp(old, "idle"))
		call_finish();
}

/* ── разбор строк модема ────────────────────────────────────────── */
static void modem_send(const char *cmd)
{
	logline("-> %s", cmd);
	char b[256];
	int n = snprintf(b, sizeof(b), "%s\r", cmd);
	if (write(modem_fd, b, n) < 0)
		logline("модем: запись не прошла");
}

/* строка в кавычках, начиная с позиции p; 0 — нет */
static int quoted(const char *s, const char **p, char *out, int n)
{
	const char *a = strchr(*p, '"');
	if (!a)
		return 0;
	const char *b = strchr(a + 1, '"');
	if (!b)
		return 0;
	int len = (int)(b - a - 1);
	if (len > n - 1)
		len = n - 1;
	memcpy(out, a + 1, len);
	out[len] = 0;
	*p = b + 1;
	(void)s;
	return 1;
}

static int clcc_pending = 0, clcc_seen = 0;

static void handle(const char *line)
{
	if (!*line)
		return;
	logline("<- %s", line);

	if (!strcmp(line, "OK") && clcc_pending) {
		/* пришёл OK, а строк +CLCC не было — вызовов не осталось */
		if (!clcc_seen && strcmp(call_state, "idle"))
			set_state("idle");
		clcc_pending = clcc_seen = 0;
	}

	if (!strncmp(line, "+CRING", 6) || !strcmp(line, "RING"))
		set_state("ringing");

	if (!strncmp(line, "+CLIP:", 6)) {
		const char *p = line;
		char num[64];
		if (quoted(line, &p, num, sizeof(num)) && num[0]) {
			snprintf(call_number, sizeof(call_number), "%s", num);
			write_state("number", call_number);
		}
	}

	/* +CLCC: id,dir,stat,mode,mpty,"номер",тип */
	if (!strncmp(line, "+CLCC:", 6)) {
		int id, dir, stat, mode, mpty;
		if (sscanf(line + 6, "%d,%d,%d,%d,%d", &id, &dir, &stat, &mode,
			   &mpty) >= 3) {
			clcc_seen = 1;
			const char *p = line + 6;
			char num[64];
			if (!call_number[0] && quoted(line, &p, num, sizeof(num)) &&
			    num[0]) {
				snprintf(call_number, sizeof(call_number), "%s", num);
				write_state("number", call_number);
			}
			const char *st = "active";
			if (stat == 1 || stat == 2 || stat == 3)
				st = "dialing";
			else if (stat == 4 || stat == 5)
				st = "ringing";
			set_state(st);
		}
	}

	if (!strcmp(line, "NO CARRIER") || !strcmp(line, "BUSY") ||
	    !strcmp(line, "NO ANSWER") || !strcmp(line, "NO DIALTONE"))
		set_state("idle");

	/* уровень сигнала, который модем шлёт сам: шкала ~0..5 */
	if (!strncmp(line, "@HTCCSQ:", 8)) {
		char b[16];
		snprintf(b, sizeof(b), "%d", atoi(line + 8));
		write_state("csq", b);
	}

	/* +CSQ: n,ber; n=0..31 -> дБм = -113 + 2n */
	if (!strncmp(line, "+CSQ:", 5)) {
		int n = atoi(line + 5);
		char b[16];
		if (n < 99)
			snprintf(b, sizeof(b), "%d", -113 + 2 * n);
		else
			b[0] = 0;
		write_state("dbm", b);
	}

	/* +COPS: mode,fmt,"оператор",AcT */
	if (!strncmp(line, "+COPS:", 6)) {
		const char *p = line + 6;
		char op[64];
		if (quoted(line, &p, op, sizeof(op))) {
			int act = -1;
			const char *c = strchr(p, ',');
			if (c)
				act = atoi(c + 1);
			const char *kind = "?";
			if (act == 0 || act == 1) kind = "GSM";
			else if (act == 2) kind = "3G";
			else if (act == 3) kind = "EDGE";
			else if (act >= 4 && act <= 6) kind = "3G+";
			else if (act == 7) kind = "LTE";
			char b[96];
			snprintf(b, sizeof(b), "%s %s", kind, op);
			write_state("net", b);
		}
	}

	/* NITZ: +HTCCTZV: "гг/мм/дд,чч:мм:сс+смещение» */
	if (!strncmp(line, "+HTCCTZV:", 9)) {
		int y, mo, d, h, mi, s;
		const char *q = strchr(line, '"');
		if (q && sscanf(q + 1, "%d/%d/%d,%d:%d:%d", &y, &mo, &d, &h, &mi,
				&s) == 6) {
			struct tm tm;
			memset(&tm, 0, sizeof(tm));
			tm.tm_year = 2000 + y - 1900;
			tm.tm_mon = mo - 1;
			tm.tm_mday = d;
			tm.tm_hour = h;
			tm.tm_min = mi;
			tm.tm_sec = s;
			/* по спецификации поле местное, но этот AMSS (сверено с
			 * NTP) кладёт туда УЖЕ UTC — смещение не вычитать */
			time_t utc = timegm(&tm);
			time_t nowt = time(NULL);
			long diff = (long)(utc > nowt ? utc - nowt : nowt - utc);
			if (utc > 0 && diff > 120) {
				struct tm g;
				gmtime_r(&utc, &g);
				char cmd[128], when[32];
				strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &g);
				snprintf(cmd, sizeof(cmd),
					 "date -u -s '%s' >/dev/null 2>&1; "
					 "hwclock -w -u >/dev/null 2>&1", when);
				if (system(cmd)) { }
				logline("время синхронизировано с вышкой");
			}
		}
	}

	/* +CMTI: "MEM",индекс — пришло сообщение */
	if (!strncmp(line, "+CMTI:", 6)) {
		const char *p = line + 6;
		char mem[32] = "";
		quoted(line, &p, mem, sizeof(mem));
		const char *c = strrchr(line, ',');
		int idx = c ? atoi(c + 1) : 0;
		int fd = open(RUN "/sms_new", O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			char b[64];
			int n = snprintf(b, sizeof(b), "%s %d\n",
					 mem[0] ? mem : "SM", idx);
			if (write(fd, b, n) < 0) { }
			close(fd);
		}
	}
}

/* ── служебные команды из очереди ───────────────────────────────── */
static void internal(const char *cmd)
{
	if (!strncmp(cmd, "@route", 6)) {
		const char *a = cmd + 6;
		while (*a == ' ')
			a++;
		const char *v = "handset";
		if (*a == 'h') v = "headset";
		else if (*a == 'l' || *a == 's') v = "speaker";
		write_state("route", v);
		audio_route();
	} else if (!strncmp(cmd, "@mute", 5)) {
		const char *a = cmd + 5;
		while (*a == ' ')
			a++;
		write_state("mute", *a == '1' ? "1" : "0");
		audio_mute();
	} else if (!strncmp(cmd, "@vol", 4)) {
		int v = atoi(cmd + 4);
		if (v < 0) v = 0;
		if (v > 100) v = 100;
		audio_ioctl(AUDIO_SET_VOLUME, (unsigned)v, 0, 1);
	}
}

int main(void)
{
	/* владелец /dev/smd0 должен быть один: две копии демона отняли бы
	 * канал друг у друга и телефон остался бы без связи */
	int lock = open("/tmp/.phoned.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
		return 0;

	mkdir(RUN, 0755);
	mkdir(HISTDIR, 0755);
	if (access(DEV, F_OK) != 0)
		if (system("mknod " DEV " c 253 0 >/dev/null 2>&1")) { }
	if (system("stty -F " DEV " raw -echo >/dev/null 2>&1")) { }
	if (access(CMDFIFO, F_OK) != 0)
		mkfifo(CMDFIFO, 0666);
	write_state("state", "idle");
	write_state("number", "");
	/* трубочный динамик молчит (усилитель не включается), громкий
	 * проверен разговором — поэтому по умолчанию он */
	write_state("route", "speaker");
	write_state("mute", "0");

	modem_fd = open(DEV, O_RDWR | O_NONBLOCK);
	if (modem_fd < 0) {
		logline("нет доступа к " DEV);
		return 1;
	}
	/* O_RDWR, а не O_RDONLY: без своего пишущего конца очередь всё время
	 * сообщает «конец файла», select срабатывает вхолостую и демон
	 * съедает процессор */
	int cmd_fd = open(CMDFIFO, O_RDWR | O_NONBLOCK);
	if (cmd_fd < 0) {
		logline("нет очереди команд");
		return 1;
	}

	/* Приветствие. Первым — включить радио: после жёсткой перезагрузки
	 * прошивка модема поднимается в +CFUN: 0 («SIM не вставлена»), и ей
	 * нужно несколько секунд, чтобы прочитать SIM. Последней — посадка
	 * на 2G: на слабом 3G пейджинг теряется, WS46 модем игнорирует,
	 * работает только ручной COPS с AcT=0. */
	modem_send("AT+CFUN=1");
	sleep(6);
	static const char *GREET[] = {"ATV1", "AT+CLIP=1", "AT+CRC=1",
				      "AT+CMGF=1", "AT+CNMI=2,1,0,0,0",
				      "AT+WS46=12", "AT+COPS=1,2,\"25002\",0",
				      NULL};
	for (int i = 0; GREET[i]; i++) {
		modem_send(GREET[i]);
		usleep(400000);
	}

	char buf[4096], cmdbuf[4096];
	int blen = 0, clen = 0;
	double last_clcc = 0, last_net = 0;
	for (;;) {
		fd_set r;
		FD_ZERO(&r);
		FD_SET(modem_fd, &r);
		FD_SET(cmd_fd, &r);
		int mx = modem_fd > cmd_fd ? modem_fd : cmd_fd;
		struct timeval tv = {1, 0};
		select(mx + 1, &r, NULL, NULL, &tv);

		if (FD_ISSET(cmd_fd, &r)) {
			int n = read(cmd_fd, cmdbuf + clen, sizeof(cmdbuf) - clen - 1);
			if (n > 0) {
				clen += n;
				cmdbuf[clen] = 0;
				char *p;
				while ((p = strchr(cmdbuf, '\n'))) {
					*p = 0;
					char cmd[512];
					snprintf(cmd, sizeof(cmd), "%s", cmdbuf);
					int rest = clen - (int)(p - cmdbuf) - 1;
					memmove(cmdbuf, p + 1, rest);
					clen = rest;
					cmdbuf[clen] = 0;
					char *e = cmd + strlen(cmd);
					while (e > cmd && (e[-1] == '\r' || e[-1] == ' '))
						*--e = 0;
					if (!cmd[0])
						continue;
					if (cmd[0] == '@') {
						internal(cmd);
						continue;
					}
					/* OK от этой команды не должен сойти за
					 * пустой ответ на наш опрос CLCC */
					clcc_pending = 0;
					modem_send(cmd);
					if (!strncasecmp(cmd, "ATD", 3)) {
						/* V.250: любой символ во время
						 * набора прерывает его — молчим
						 * первые две секунды */
						last_clcc = now_s() + 2.0;
						snprintf(call_number,
							 sizeof(call_number), "%s",
							 cmd + 3);
						char *s = strchr(call_number, ';');
						if (s)
							*s = 0;
						write_state("number", call_number);
						set_state("dialing");
					} else if (!strncasecmp(cmd, "ATA", 3) ||
						   !strncasecmp(cmd, "ATH", 3)) {
						last_clcc = now_s() + 2.0;
					}
				}
				if (clen >= (int)sizeof(cmdbuf) - 1)
					clen = 0;      /* мусор — выбрасываем */
			}
		}

		if (FD_ISSET(modem_fd, &r)) {
			int n = read(modem_fd, buf + blen, sizeof(buf) - blen - 1);
			if (n > 0) {
				blen += n;
				buf[blen] = 0;
				char *p;
				while ((p = strchr(buf, '\n'))) {
					*p = 0;
					char line[1024];
					snprintf(line, sizeof(line), "%s", buf);
					int rest = blen - (int)(p - buf) - 1;
					memmove(buf, p + 1, rest);
					blen = rest;
					buf[blen] = 0;
					char *e = line + strlen(line);
					while (e > line && (e[-1] == '\r' ||
							    e[-1] == ' '))
						*--e = 0;
					handle(line);
				}
				if (blen >= (int)sizeof(buf) - 1)
					blen = 0;
			}
		}

		double t = now_s();
		/* пока вызов жив — сами держим состояние в курсе */
		if (strcmp(call_state, "idle") && t - last_clcc > 2.0) {
			last_clcc = t;
			clcc_pending = 1;
			clcc_seen = 0;
			modem_send("AT+CLCC");
		}
		/* в покое раз в 25 с спрашиваем сигнал и тип сети */
		if (!strcmp(call_state, "idle") && t - last_net > 25.0) {
			last_net = t;
			modem_send("AT+CSQ");
			modem_send("AT+COPS?");
		}
		ringer_tick(!strcmp(call_state, "ringing"));
	}
	return 0;
}
