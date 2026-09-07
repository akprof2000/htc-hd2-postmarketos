/* btsdp — спросить у устройства, какие службы оно предлагает и на каком
 * канале RFCOMM их слушать. Нужен для гарнитуры: без номера канала
 * подключаться некуда.
 *
 * Основание (сырой HCI, сопряжение, свой L2CAP) взято из bta2dp.c —
 * рабочий A2DP не трогаем, поэтому копия, а не общий файл. Переменные
 * окружения те же: BTA2DP_PSRM, BTA2DP_ALTADDR, BTA2DP_NOAUTH.
 *
 *     btsdp <MAC>
 *
 * Сборка: gcc -O2 btsdp.c -o btsdp
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sbc/sbc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define AF_BLUETOOTH_ 31
#define BTPROTO_HCI_  1
#define HCI_FILTER_   2
#define AVDTP_PSM     25

struct sockaddr_hci_ { unsigned short family, dev, channel; };

/* ── состояние ─────────────────────────────────────────────────────── */
static int hci = -1;
static unsigned char dst[6];
static unsigned short handle = 0;
static int acl_mtu = 1021, credits = 6;
static int debug = 0;
/* номера НАШИХ каналов (см. sig_scid): по ним отличаем свои сигналы
 * от сигналов к каналам ядра */
static unsigned short my_scids[4] = { 0x0070, 0x0071, 0, 0 };

static void hexline(const char *what, const unsigned char *d, int n)
{
	if (!debug)
		return;
	printf("  %s:", what);
	for (int i = 0; i < n && i < 40; i++)
		printf(" %02x", d[i]);
	printf("%s\n", n > 40 ? " …" : "");
	fflush(stdout);
}

/* принятые L2CAP-кадры, по одному на канал (CID) */
struct frame { unsigned short cid; int len; unsigned char data[2048]; };
#define QMAX 16
static struct frame q[QMAX];
static int qn = 0;

/* сборка фрагментов ACL */
static unsigned char asm_buf[4096];
static int asm_len = 0, asm_want = 0;

static double now_s(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void say(const char *s)
{
	printf("%s\n", s);
	fflush(stdout);
}

/* ── HCI ───────────────────────────────────────────────────────────── */
#include <sys/ioctl.h>
#define HCISETRAW_ 0x400448dc                 /* _IOW('H', 220, int) */
static int raw_on = 0;
static int fifo_fd = -1;                    /* канал от плеера */
static void raw_mode(int on)
{
	struct { unsigned short dev_id; unsigned dev_opt; } dr = { 0, (unsigned)on };
	if (ioctl(hci, HCISETRAW_, &dr) < 0)
		printf("сырой режим %s: %s%c", on ? "вкл" : "выкл", strerror(errno), 10);
	else
		raw_on = on;
}

static void send_cmd(unsigned short op, const unsigned char *pl, int n)
{
	unsigned char pkt[300];
	pkt[0] = 0x01;
	pkt[1] = op & 0xff;
	pkt[2] = op >> 8;
	pkt[3] = n;
	if (n)
		memcpy(pkt + 4, pl, n);
	if (write(hci, pkt, 4 + n) < 0)
		perror("write hci");
}

/* Ключи связи. Ядро без интерфейса mgmt их не хранит и на запрос ключа
 * молчит, поэтому храним сами: колонка после первого сопряжения при
 * каждом новом подключении спрашивает ключ, и на «ключа нет» рвёт
 * канал с кодом 0x06. Файл: строки «адрес 32 hex-знака ключа». */
#define KEYS_FILE "/root/.btkeys"

static void mac_str(const unsigned char *raw, char *out)
{
	sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", raw[5], raw[4], raw[3],
		raw[2], raw[1], raw[0]);
}

static int key_load(const unsigned char *raw, unsigned char *key)
{
	char want[18];
	mac_str(raw, want);
	FILE *f = fopen(KEYS_FILE, "r");
	if (!f)
		return 0;
	char m[32], hex[64];
	int found = 0;
	while (fscanf(f, "%31s %63s", m, hex) == 2) {
		if (strcmp(m, want) || strlen(hex) != 32)
			continue;
		for (int i = 0; i < 16; i++) {
			unsigned v;
			sscanf(hex + i * 2, "%2x", &v);
			key[i] = v;
		}
		found = 1;                          /* последний — главнее */
	}
	fclose(f);
	return found;
}

static void key_save(const unsigned char *raw, const unsigned char *key)
{
	char m[18];
	mac_str(raw, m);
	FILE *f = fopen(KEYS_FILE, "a");
	if (!f)
		return;
	fprintf(f, "%s ", m);
	for (int i = 0; i < 16; i++)
		fprintf(f, "%02x", key[i]);
	fprintf(f, "%c", 10);
	fclose(f);
	printf("ключ связи с %s сохранён%c", m, 10);
}

/* ответы на запросы сопряжения — то же, что делает btagent */
static void pairing_events(int code, const unsigned char *b, int n)
{
	unsigned char p[32];
	switch (code) {
	case 0x17: {                                /* Link Key Request */
		unsigned char key[16];
		if (key_load(b, key)) {
			memcpy(p, b, 6);
			memcpy(p + 6, key, 16);
			send_cmd(0x040b, p, 22);        /* вот наш ключ */
		} else
			send_cmd(0x040c, b, 6);         /* ключа нет — сопряжение */
		break;
	}
	case 0x18:                                  /* Link Key Notification */
		if (n >= 22)
			key_save(b, b + 6);
		break;
	case 0x16:                                  /* PIN Code Request */
		memcpy(p, b, 6);
		p[6] = 4;
		memset(p + 7, 0, 16);
		memcpy(p + 7, "0000", 4);
		send_cmd(0x040d, p, 23);
		break;
	case 0x31:                                  /* IO Capability Request */
		memcpy(p, b, 6);
		/* NoInputNoOutput; требования — «общее сопряжение» (0x04),
		 * как у телефонов: с «без запоминания» (0x00) колонка после
		 * обмена IO Capability замолкала и ключа не выдавала */
		p[6] = 0x03; p[7] = 0x00; p[8] = 0x04;
		send_cmd(0x042b, p, 9);
		break;
	case 0x33:                                  /* User Confirmation */
		send_cmd(0x042c, b, 6);
		break;
	default:
		break;
	}
}

static void queue_frame(unsigned short cid, const unsigned char *d, int len)
{
	if (qn >= QMAX || len > (int)sizeof(q[0].data))
		return;
	q[qn].cid = cid;
	q[qn].len = len;
	memcpy(q[qn].data, d, len);
	qn++;
}

/* один пакет из сокета; события сопряжения и кредиты — сразу,
 * L2CAP-кадры — в очередь. Возвращает код события или 0 */
static int pump(int timeout_ms)
{
	struct pollfd pf = { hci, POLLIN, 0 };
	if (poll(&pf, 1, timeout_ms) <= 0)
		return 0;
	unsigned char d[4096];
	int r = read(hci, d, sizeof(d));
	if (r < 1)
		return 0;
	if (d[0] == 0x04 && r >= 3) {                /* событие */
		int code = d[1];
		const unsigned char *b = d + 3;
		int n = r - 3;
		if (code != 0x13)
			hexline("событие", d + 1, r - 1);
		if (code == 0x0e && n >= 3 && b[1] == 0x05 && b[2] == 0x10
		    && n >= 10) {                        /* Read Buffer Size */
			if (b[3] == 0x00) {
				acl_mtu = b[4] | (b[5] << 8);
				credits = b[7] | (b[8] << 8);
				printf("буферы контроллера: пакет %d байт, мест %d\n",
				       acl_mtu, credits);
				fflush(stdout);
			}
		}
		if (code == 0x0f && n >= 4 && b[0] != 0x00) {  /* Command Status */
			printf("контроллер отверг команду 0x%04x (код 0x%02x)\n",
			       b[2] | (b[3] << 8), b[0]);
			fflush(stdout);
		}
		if (code == 0x12 && n >= 8) {           /* Role Change */
			printf("роль сменилась: мы %s (код %d)\n",
			       b[7] ? "ведомые" : "ведущие", b[0]);
			fflush(stdout);
		}
		if (code == 0x0e && n >= 7 && b[1] == 0x09 && b[2] == 0x08) {
			printf("роль в паре: мы %s\n",
			       b[6] ? "ведомые" : "ведущие");
			fflush(stdout);
		}
		if (code == 0x1b && n >= 3) {           /* Max Slots Change */
			printf("максимум слотов в пакете: %d\n", b[2]);
			fflush(stdout);
		}
		if (code == 0x1d && n >= 5) {           /* Packet Type Changed */
			printf("типы пакетов канала: 0x%04x (код %d)\n",
			       b[3] | (b[4] << 8), b[0]);
			fflush(stdout);
		}
		if (code == 0x13 && n >= 5) {           /* Number of Completed */
			int k = b[0];
			for (int i = 0; i < k; i++)
				credits += b[1 + i * 4 + 2] | (b[1 + i * 4 + 3] << 8);
		} else if (code == 0x14 && n >= 6) {   /* Mode Change */
			/* Колонка уводит канал в дремоту (sniff) уже после
			 * настройки, и тогда контроллер отдаёт всего ~19 пакетов
			 * в секунду вместо нужных 70 — звук рвётся. Возвращаем
			 * канал в активный режим каждый раз, как это заметим. */
			if (b[3] != 0x00) {
				unsigned char h2[2] = { b[1], b[2] };
				send_cmd(0x0804, h2, 2);        /* Exit Sniff Mode */
				unsigned char lp[4] = { b[1], b[2], 0x00, 0x00 };
				send_cmd(0x080d, lp, 4);        /* и запрет на будущее */
				say("канал ушёл в дремоту — вывожу обратно");
			}
		} else if (code == 0x05 && n >= 4) {   /* Disconnection */
			unsigned short h = b[1] | (b[2] << 8);
			if (h == handle) {
				printf("колонка разорвала соединение (код 0x%02x)\n",
				       b[3]);
				handle = 0;
			}
		} else
			pairing_events(code, b, n);
		return code;
	}
	if (d[0] == 0x02 && r >= 5) {                /* ACL-данные */
		unsigned short h = (d[1] | (d[2] << 8));
		int pb = (h >> 12) & 3;
		h &= 0x0fff;
		int len = d[3] | (d[4] << 8);
		if (h != handle || len != r - 5)
			return 0;
		if (pb == 2 || pb == 0) {               /* первый фрагмент */
			if (len < 4)
				return 0;
			asm_want = (d[5] | (d[6] << 8)) + 4;
			asm_len = 0;
		}
		if (asm_len + len > (int)sizeof(asm_buf))
			return 0;
		memcpy(asm_buf + asm_len, d + 5, len);
		asm_len += len;
		if (asm_len >= asm_want && asm_want >= 4) {
			unsigned short cid = asm_buf[2] | (asm_buf[3] << 8);
			hexline("<- L2CAP", asm_buf, asm_want);
			queue_frame(cid, asm_buf + 4, asm_want - 4);
			asm_len = asm_want = 0;
		}
		return 0x100;
	}
	return 0;
}

static double t_wait = 0;      /* сколько всего ждали кредитов */
static long n_pkt = 0;         /* сколько пакетов ушло */
static long n_byte = 0;

/* отправка L2CAP-кадра в канал; ждём кредит контроллера */
static int l2_send(unsigned short cid, const unsigned char *d, int len)
{
	/* Ждём кредит контроллера. Слать «через силу» нельзя: буферы
	 * переполняются, пакеты пропадают, и колонка вместо музыки
	 * получает обрывки — проверено, звук пропал совсем. */
	double tw0 = now_s();
	while (credits <= 0) {
		if (!pump(1000)) {
			say("контроллер не возвращает кредиты");
			return -1;
		}
	}
	t_wait += now_s() - tw0;
	unsigned char pkt[2048];
	int l2 = len + 4;
	if (l2 + 5 > (int)sizeof(pkt) || l2 > acl_mtu) {
		say("пакет больше ACL MTU");
		return -1;
	}
	pkt[0] = 0x02;
	pkt[1] = handle & 0xff;
	pkt[2] = ((handle >> 8) & 0x0f) | 0x20;    /* PB = первый пакет */
	pkt[3] = l2 & 0xff;
	pkt[4] = l2 >> 8;
	pkt[5] = len & 0xff;
	pkt[6] = len >> 8;
	pkt[7] = cid & 0xff;
	pkt[8] = cid >> 8;
	memcpy(pkt + 9, d, len);
	hexline("-> L2CAP", pkt + 5, len + 4);
	credits--;
	n_pkt++;
	n_byte += 9 + len;
	if (write(hci, pkt, 9 + len) < 0) {
		perror("write acl");
		return -1;
	}
	return 0;
}

/* забрать кадр нужного канала из очереди (ждём до timeout) */
static int l2_recv(unsigned short cid, unsigned char *out, int max,
		   int timeout_ms)
{
	double end = now_s() + timeout_ms / 1000.0;
	for (;;) {
		for (int i = 0; i < qn; i++) {
			if (q[i].cid != cid)
				continue;
			int n = q[i].len < max ? q[i].len : max;
			memcpy(out, q[i].data, n);
			memmove(&q[i], &q[i + 1], (qn - i - 1) * sizeof(q[0]));
			qn--;
			return n;
		}
		if (now_s() > end || !handle)
			return -1;
		pump(200);
	}
}

/* ── L2CAP сигнализация (CID 1) ────────────────────────────────────── */
static unsigned char sig_id = 1;

static void l2_sig(unsigned char code, unsigned char id,
		   const unsigned char *d, int n)
{
	unsigned char b[64];
	b[0] = code; b[1] = id; b[2] = n & 0xff; b[3] = n >> 8;
	memcpy(b + 4, d, n);
	l2_send(0x0001, b, 4 + n);
}

/* Ответы на сигналы устройства. Отвечаем ТОЛЬКО за свои каналы:
 * Information/Echo Request и Config Request к каналам ядра (SDP-
 * держатель) ядро обслуживает само; наш второй ответ ему бы мешал. */
static int is_my_cid(unsigned short cid)
{
	for (int i = 0; i < 4; i++)
		if (my_scids[i] && my_scids[i] == cid)
			return 1;
	return 0;
}

static void l2_handle_peer_sig(const unsigned char *f, int n)
{
	if (n < 4)
		return;
	int code = f[0], id = f[1];
	if (code == 0x04 && n >= 8) {               /* Config Request */
		unsigned short dcid = f[4] | (f[5] << 8);
		if (!is_my_cid(dcid))
			return;
		unsigned char r[6] = { f[4], f[5], 0, 0, 0, 0 };
		l2_sig(0x05, id, r, 6);
	} else if (code == 0x06 && n >= 8) {        /* Disconnect Request */
		unsigned short dcid = f[4] | (f[5] << 8);
		if (!is_my_cid(dcid))
			return;
		unsigned char r[4] = { f[4], f[5], f[6], f[7] };
		l2_sig(0x07, id, r, 4);
	}
}

/* открыть канал к PSM; возвращает DCID (канал колонки) или 0 */
static unsigned short l2_open(unsigned short psm, unsigned short scid,
			      int *out_mtu)
{
	unsigned char req[4] = { psm & 0xff, psm >> 8, scid & 0xff, scid >> 8 };
	unsigned char myid = sig_id++;
	l2_sig(0x02, myid, req, 4);                /* Connect Request */
	unsigned short dcid = 0;
	int conf_sent = 0, conf_ok = 0, peer_conf = 0;
	/* 25 секунд: при первом подключении колонка сначала проводит
	 * сопряжение (несколько секунд) и наш запрос канала может
	 * потерять — повторяем его каждые 6 с, пока нет ответа */
	double end = now_s() + 25, resend = now_s() + 6;
	*out_mtu = 672;
	while (now_s() < end && handle) {
		if (!dcid && now_s() > resend) {
			myid = sig_id++;
			l2_sig(0x02, myid, req, 4);
			resend = now_s() + 6;
		}
		unsigned char f[256];
		int n = l2_recv(0x0001, f, sizeof(f), 500);
		if (n < 4)
			continue;
		int code = f[0], id = f[1];
		if (code == 0x03 && n >= 12 && id == myid) {   /* Connect Response */
			unsigned short rs = f[4] | (f[5] << 8);
			int result = f[8] | (f[9] << 8);
			if (result == 1)                    /* pending */
				continue;
			if (result != 0) {
				printf("PSM %d: отказ, результат %d\n", psm, result);
				return 0;
			}
			dcid = rs;
			/* наш Config Request: MTU 672 */
			unsigned char c[8] = { dcid & 0xff, dcid >> 8, 0, 0,
					       0x01, 0x02, 0x7f, 0x03 };  /* MTU 895 */
			myid = sig_id++;
			l2_sig(0x04, myid, c, 8);
			conf_sent = 1;
		} else if (code == 0x05 && n >= 10 && conf_sent) {  /* Config Response */
			int result = f[8] | (f[9] << 8);
			if (result != 0) {
				printf("PSM %d: конфигурация отвергнута (%d)\n", psm,
				       result);
				return 0;
			}
			conf_ok = 1;
		} else if (code == 0x04 && n >= 8 &&
			   (f[4] | (f[5] << 8)) == scid) {     /* его Config Request к нам */
			for (int i = 8; i + 1 < n;) {
				int t = f[i] & 0x7f, l = f[i + 1];
				if (t == 0x01 && l == 2 && i + 3 < n)
					*out_mtu = f[i + 2] | (f[i + 3] << 8);
				i += 2 + l;
			}
			l2_handle_peer_sig(f, n);
			peer_conf = 1;
		} else
			l2_handle_peer_sig(f, n);
		if (dcid && conf_ok && peer_conf)
			return dcid;
	}
	printf("PSM %d: канал не открылся за 25 с\n", psm);
	return 0;
}

/* ── AVDTP ─────────────────────────────────────────────────────────── */
enum { DISCOVER = 1, GET_CAPS = 2, SET_CONF = 3, OPEN = 6, START = 7,
       CLOSE = 8, SUSPEND = 9 };
/* Номера наших каналов — с 0x0070: ядро для своих (SDP-держатель)
 * берёт 0x0040 и дальше, совпадение номеров устройство не прощает. */
static unsigned short sig_dcid = 0, sig_scid = 0x0070;
static unsigned char label = 0;

static void hangup(void);
/* SIGTERM/SIGINT: speaker off и сторожа убивают нас сигналом; без
 * разрыва ACL колонка остаётся «подключённой» к мёртвому процессу, и
 * следующий вызов контроллер отвергает с 0x0b («канал уже есть»). */
static void on_signal(int sig)
{
	(void)sig;
	say("сигнал — разрываю и выхожу");
	hangup();
	_exit(0);
}

static void hangup(void)
{
	if (handle) {
		unsigned char dc[3] = { handle & 0xff, handle >> 8, 0x13 };
		send_cmd(0x0406, dc, 3);
		usleep(300000);
	}
	if (getenv("BTA2DP_ALTADDR")) {
		unsigned char addr[6] = { 0x89, 0x4b, 0x45, 0x93, 0x61, 0x7c };
		send_cmd(0xfc01, addr, 6);
		usleep(200000);
		send_cmd(0x0c03, NULL, 0);
		usleep(500000);
		unsigned char se[1] = { 0x03 };
		send_cmd(0x0c1a, se, 1);
		usleep(100000);
	}
	if (raw_on)
		raw_mode(0);
	close(hci);
}

/* ── разбор описаний SDP ───────────────────────────────────────────── */
#define SDP_PSM 1

/* Заголовок элемента: возвращает его длину в байтах, а тип и длину
 * данных кладёт по указателям. Отрицательный ответ — данные кончились. */
static int sdp_hdr(const unsigned char *d, int n, int *type, int *len)
{
	if (n < 1)
		return -1;
	int t = d[0] >> 3, si = d[0] & 7, hdr = 1, l = 0;
	switch (si) {
	case 0: l = (t == 0) ? 0 : 1; break;   /* «ничего» либо один байт */
	case 1: l = 2; break;
	case 2: l = 4; break;
	case 3: l = 8; break;
	case 4: l = 16; break;
	case 5: if (n < 2) return -1; l = d[1]; hdr = 2; break;
	case 6: if (n < 3) return -1; l = (d[1] << 8) | d[2]; hdr = 3; break;
	default: if (n < 5) return -1;
		l = (d[1] << 24) | (d[2] << 16) | (d[3] << 8) | d[4];
		hdr = 5; break;
	}
	if (l < 0 || hdr + l > n)
		return -1;
	*type = t;
	*len = l;
	return hdr;
}

/* Пройти по парам «номер атрибута — значение» внутри записи и найти
 * значение нужного атрибута. */
static const unsigned char *sdp_attr(const unsigned char *rec, int n,
				     int want, int *out_len)
{
	int p = 0;
	while (p < n) {
		int t, l, h = sdp_hdr(rec + p, n - p, &t, &l);
		if (h < 0)
			return NULL;
		/* номер атрибута — беззнаковое целое в два байта */
		int id = -1;
		if (t == 1 && l == 2)
			id = (rec[p + h] << 8) | rec[p + h + 1];
		p += h + l;
		if (p >= n)
			return NULL;
		int vt, vl, vh = sdp_hdr(rec + p, n - p, &vt, &vl);
		if (vh < 0)
			return NULL;
		if (id == want) {
			*out_len = vl;
			return rec + p + vh;
		}
		p += vh + vl;
	}
	return NULL;
}

/* Номер канала RFCOMM: в списке протоколов лежит пара «UUID 0x0003,
 * номер канала». Ищем её прямо по байтам — вложенность там глубокая, а
 * образец однозначный. */
static int rfcomm_channel(const unsigned char *rec, int n)
{
	int vl;
	const unsigned char *v = sdp_attr(rec, n, 0x0004, &vl);
	if (!v) { v = rec; vl = n; }
	for (int i = 0; i + 4 < vl; i++)
		if (v[i] == 0x19 && v[i + 1] == 0x00 && v[i + 2] == 0x03 &&
		    v[i + 3] == 0x08)
			return v[i + 4];
	return -1;
}

/* Класс службы: первый UUID в списке классов. */
static int sdp_service_class(const unsigned char *rec, int n)
{
	int vl;
	const unsigned char *v = sdp_attr(rec, n, 0x0001, &vl);
	if (!v || vl < 3 || v[0] != 0x19)
		return -1;
	return (v[1] << 8) | v[2];
}

/* Имя службы, если устройство его назвало. */
static void sdp_service_name(const unsigned char *rec, int n,
			     char *out, int max)
{
	out[0] = 0;
	int vl;
	const unsigned char *v = sdp_attr(rec, n, 0x0100, &vl);
	if (!v)
		return;
	if (vl > max - 1)
		vl = max - 1;
	memcpy(out, v, vl);
	out[vl] = 0;
}

/* ── main ──────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
	unsigned v[6];
	if (argc < 2 || sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &v[0], &v[1],
			       &v[2], &v[3], &v[4], &v[5]) != 6) {
		say("использование: bta2dp <MAC> [файл.wav | -]");
		return 2;
	}
	for (int i = 0; i < 6; i++)
		dst[i] = (unsigned char)v[5 - i];
	signal(SIGPIPE, SIG_IGN);
	/* Канал плеера открываем ПЕРВЫМ ДЕЛОМ, до дозвона: пока читателя
	 * нет, плеер либо висит в open(), либо ловит SIGPIPE на записи и
	 * умирает — так и случилось. На чтение-запись, чтобы конец потока
	 * не наступал на паузах. */
	if (argc > 2 && (!strncmp(argv[2], "/run/", 5) || strstr(argv[2], ".pcm"))) {
		fifo_fd = open(argv[2], O_RDWR);
		if (fifo_fd < 0) {
			printf("нет канала %s%c", argv[2], 10);
			return 1;
		}
		printf("канал %s открыт, жду колонку%c", argv[2], 10);
		fflush(stdout);
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	debug = getenv("BTA2DP_DEBUG") != NULL;

	hci = socket(AF_BLUETOOTH_, SOCK_RAW, BTPROTO_HCI_);
	if (hci < 0) {
		say("нет доступа к HCI");
		return 1;
	}
	/* события и ACL-данные, все коды событий */
	unsigned f[4] = { (1u << 4) | (1u << 2), 0xffffffff, 0xffffffff, 0 };
	setsockopt(hci, 0, HCI_FILTER_, f, sizeof(f));
	struct sockaddr_hci_ a = { AF_BLUETOOTH_, 0, 0 };
	if (bind(hci, (struct sockaddr *)&a, sizeof(a)) < 0) {
		say("не удалось привязаться к hci0");
		return 1;
	}
	/* СЫРОЙ РЕЖИМ (HCISETRAW). ПРОВЕРЕНО 05.09 на наушниках: без него
	 * ядро, не зная наших L2CAP-каналов, отвечает устройству Command
	 * Reject на его же Connect Response и Config Request — и устройство
	 * закрывает только что открытый канал. В сыром режиме ядро только
	 * раздаёт пакеты сокетам и само не обрабатывает ничего: ни L2CAP,
	 * ни hci_conn с его таймером разрыва. Кредиты и сопряжение мы и так
	 * ведём сами. На выходе режим снимаем. */
	raw_mode(1);

	/* Другой адрес на время сеанса (BTA2DP_ALTADDR). Колонка помнит
	 * ключ от старого сопряжения, которого у нас нет, и на «ключа нет»
	 * рвёт канал даже в режиме сопряжения. С новым адресом мы для неё
	 * новое устройство — сопряжение идёт заново, ключ сохраняем. Адрес
	 * пишется командой Broadcom 0xFC01 и вступает после HCI Reset. */
	if (getenv("BTA2DP_ALTADDR")) {
		unsigned char addr[6] = { 0x8a, 0x4b, 0x45, 0x93, 0x61, 0x7c };
		send_cmd(0xfc01, addr, 6);
		usleep(200000);
		send_cmd(0x0c03, NULL, 0);                /* HCI Reset */
		usleep(500000);
		unsigned char se[1] = { 0x03 };
		send_cmd(0x0c1a, se, 1);                  /* scan enable */
		usleep(100000);
		say("представляюсь адресом 7C:61:93:45:4B:8A");
	}

	/* 1. ACL-соединение в режиме R0 */
	unsigned char cc[13];
	memcpy(cc, dst, 6);
	cc[6] = 0x18; cc[7] = 0xcc;                  /* DM1..DH5 */
	/* Режим страничного сканирования и смещение часов — из кэша поиска
	 * (/run/btinq, пишет btscan), последняя запись главнее. Без них
	 * наушники и колонка на вызов не отвечали. BTA2DP_PSRM перебивает
	 * режим вручную. */
	cc[8] = 0x00;
	{
		char want[18];
		mac_str(dst, want);
		FILE *cf = fopen("/run/btinq", "r");
		if (cf) {
			char m[32];
			int psrm, co;
			while (fscanf(cf, "%31s %d %d", m, &psrm, &co) == 3) {
				if (strcmp(m, want))
					continue;
				cc[8] = (unsigned char)psrm;
				cc[10] = co & 0xff;
				cc[11] = ((co >> 8) & 0x7f) | 0x80;   /* смещение верно */
			}
			fclose(cf);
		}
		if (getenv("BTA2DP_PSRM"))
			cc[8] = (unsigned char)atoi(getenv("BTA2DP_PSRM"));
	}
	cc[9] = 0; cc[10] = 0; cc[11] = 0;
	cc[12] = 0x01;                               /* смену роли разрешаем: колонки
	                                              * любят быть главными, а
	                                              * без этого JBL молчала */
	int retried = 0;
	send_cmd(0x0405, cc, 13);
	printf("вызываю устройство (R%d, смещение часов %s)…%c", cc[8],
	       cc[11] & 0x80 ? "известно" : "неизвестно", 10);
	fflush(stdout);
	double end = now_s() + 30;
	while (now_s() < end && !handle) {
		unsigned char d[300];
		struct pollfd pf = { hci, POLLIN, 0 };
		if (poll(&pf, 1, 500) <= 0)
			continue;
		int r = read(hci, d, sizeof(d));
		if (r < 3 || d[0] != 0x04)
			continue;
		const unsigned char *b = d + 3;
		if (d[1] == 0x03 && r >= 12) {           /* Connection Complete */
			if (b[0]) {
				printf("колонка не ответила на вызов (код 0x%02x)\n",
				       b[0]);
				if (raw_on) raw_mode(0);
		close(hci);
				return 1;
			}
			handle = b[1] | (b[2] << 8);
		} else if (d[1] == 0x0f && r >= 7 && b[0]) {
			if (b[0] == 0x0b && !retried) {
				/* «канал уже есть» — остался от убитого процесса;
				 * рвём возможные ручки и зовём ещё раз */
				say("канал уже есть — рву старый и зову снова");
				for (int h = 0x0b; h <= 0x0e; h++) {
					unsigned char dc[3] = { (unsigned char)h, 0, 0x13 };
					send_cmd(0x0406, dc, 3);
					usleep(150000);
				}
				retried = 1;
				send_cmd(0x0405, cc, 13);
				continue;
			}
			printf("контроллер отверг вызов (код 0x%02x)\n", b[0]);
			if (raw_on) raw_mode(0);
			close(hci);
			return 1;
		} else
			pairing_events(d[1], b, r - 3);
	}
	if (!handle) {
		say("колонка не ответила за 30 с — отменяю вызов");
		send_cmd(0x0408, dst, 6);
		usleep(500000);
		if (raw_on) raw_mode(0);
		close(hci);
		return 1;
	}
	printf("ACL-канал установлен, ручка %d\n", handle);
	fflush(stdout);
	usleep(300000);

	/* Запрещаем каналу «дремать». Колонка переводит его в режим sniff
	 * (данные только в редкие окна), и контроллер отдавал всего ~19
	 * пакетов в секунду вместо нужных ~70 — звук выходил рваным.
	 * Write Link Policy Settings = 0 запрещает sniff/hold/park, а
	 * Exit Sniff Mode выводит из него, если колонка уже увела канал. */
	{
		/* Кто в паре главный. Если главная колонка, мы передаём только
		 * когда она нас опросит, а опрашивает она редко — поток и
		 * упирался в 10.8 КБ/с (предел односотовых пакетов). Просим
		 * главенство себе: сперва разрешаем смену роли в политике,
		 * потом Switch Role, и лишь затем запрещаем дремоту. */
		if (!getenv("BTA2DP_NOMASTER")) {
			unsigned char lp0[4] = { handle & 0xff, handle >> 8,
						 0x01, 0x00 };  /* смена роли можно */
			send_cmd(0x080d, lp0, 4);
			usleep(50000);
			unsigned char sr[7];
			memcpy(sr, dst, 6);
			sr[6] = 0x00;                   /* хотим быть ведущими */
			send_cmd(0x080b, sr, 7);
			for (int i = 0; i < 40; i++)
				pump(50);
			unsigned char rd[2] = { handle & 0xff, handle >> 8 };
			send_cmd(0x0809, rd, 2);
			for (int i = 0; i < 10; i++)
				pump(30);
		}
		/* Сколько мест в очереди контроллера и какой у него предел
		 * пакета — раньше мы просто верили в 1021/6. */
		send_cmd(0x1005, NULL, 0);
		for (int i = 0; i < 10; i++)
			pump(30);
		/* 0x080d — политика ЭТОГО канала (0x080f была бы «по
		 * умолчанию», и параметры у неё другие: с ней команда просто
		 * отвергалась, а канал продолжал дремать) */
		unsigned char lp[4] = { handle & 0xff, handle >> 8, 0x00, 0x00 };
		send_cmd(0x080d, lp, 4);
		usleep(50000);
		unsigned char dp[2] = { 0x00, 0x00 };   /* и для будущих каналов */
		send_cmd(0x080f, dp, 2);
		usleep(50000);
		unsigned char h2[2] = { handle & 0xff, handle >> 8 };
		send_cmd(0x0804, h2, 2);
		usleep(50000);
		/* Разрешаем каналу крупные пакеты (DM1..DH5). Скорость держалась
		 * около 11 КБ/с — это предел односотовых DM1; в Create Connection
		 * типы заданы, но контроллер мог сузить их при установлении. */
		unsigned char pt[4] = { handle & 0xff, handle >> 8, 0x18, 0xcc };
		send_cmd(0x040f, pt, 4);
		usleep(100000);
	}

	/* Аутентификацию начинаем САМИ, до L2CAP. Иначе её начинает колонка:
	 * спрашивает ключ связи, и если ключа нет (первое сопряжение не
	 * досидели или его сделал старый код без хранилища), рвёт канал с
	 * кодом 0x06 — сопряжение заново она при этом не предлагает. Когда
	 * же аутентификацию просим мы, контроллер при отсутствии ключа сам
	 * запускает SSP-сопряжение, колонка в режиме сопряжения его
	 * принимает, и ключ приходит нам (Link Key Notification). */
	if (!getenv("BTA2DP_NOAUTH")) {
		unsigned char h[2] = { handle & 0xff, handle >> 8 };
		send_cmd(0x0411, h, 2);                  /* Authentication Requested */
		double until = now_s() + 20;
		int done = 0;
		while (now_s() < until && handle && !done) {
			int ev = pump(500);
			if (ev == 0x06) {                    /* Authentication Complete */
				say("аутентификация завершена");
				done = 1;
			}
		}
		if (!handle) {
			say("колонка оборвала канал во время аутентификации");
			if (raw_on) raw_mode(0);
		close(hci);
			return 1;
		}
		if (!done)
			say("аутентификация не подтвердилась за 20 с — пробую канал всё равно");
	}

	/* Ядро завело у себя запись об этом ACL (по Command Status нашего
	 * вызова) и, раз в ядре её никто не держит, через несколько секунд
	 * само рвёт канал (Disconnect, код 0x16 «локальным хостом»). Даём
	 * ему повод держать: открываем ЧЕРЕЗ ЯДРО безобидный L2CAP-канал к
	 * SDP (PSM 1, без аутентификации) и не закрываем до конца работы.
	 * Ядро при этом страничный вызов не делает — канал уже есть. */
	if (!raw_on) {
		int k = socket(AF_BLUETOOTH_, SOCK_SEQPACKET, 0 /* L2CAP */);
		if (k >= 0) {
			struct { unsigned short family, psm; unsigned char bd[6];
				 unsigned short cid; } la;
			memset(&la, 0, sizeof(la));
			la.family = AF_BLUETOOTH_;
			bind(k, (struct sockaddr *)&la, sizeof(la));
			la.psm = 1;
			memcpy(la.bd, dst, 6);
			struct timeval sto = {15, 0};
			setsockopt(k, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof(sto));
			/* connect блокирует до 15 с; события сопряжения за это время
			 * ядро не трогает (без mgmt молчит), а нам они и не нужны —
			 * SDP идёт без аутентификации */
			if (connect(k, (struct sockaddr *)&la, sizeof(la)) == 0)
				say("ядро держит канал (SDP)");
			else {
				printf("SDP через ядро: %s — держателя нет%c",
				       strerror(errno), 10);
				close(k);
			}
		}
	}

	/* Режим «держать»: только ACL и ответы на сопряжение, без L2CAP —
	 * для проверки, подхватит ли ядро готовый канал своим L2CAP. */
	if (getenv("BTA2DP_HOLD")) {
		say("держу канал 60 с");
		double hold = now_s() + 60;
		while (now_s() < hold && handle)
			pump(500);
		hangup();
		return 0;
	}

	/* 2. канал SDP и опрос служб */
	int mtu_sig;
	sig_dcid = l2_open(SDP_PSM, sig_scid, &mtu_sig);
	if (!sig_dcid) {
		hangup();
		return 1;
	}
	printf("канал SDP открыт, MTU %d\n", mtu_sig);
	fflush(stdout);

	/* Спрашиваем разом обе службы гарнитуры: Handsfree (0x111E) и
	 * Headset (0x1108). Запрашиваем все атрибуты (0x0000..0xFFFF) —
	 * оттуда возьмём имя службы и номер канала RFCOMM. */
	static unsigned char blob[8192];
	int blob_len = 0;
	unsigned char cont[17] = { 0 };
	int cont_len = 0;
	unsigned short tid = 1;

	for (int round = 0; round < 40; round++) {
		unsigned char req[64], *p = req;
		*p++ = 0x06;                        /* ServiceSearchAttribute */
		*p++ = tid >> 8; *p++ = tid & 0xff;
		unsigned char *plen = p; p += 2;    /* длину впишем потом */
		/* образец поиска: две службы */
		*p++ = 0x35; *p++ = 0x06;
		*p++ = 0x19; *p++ = 0x11; *p++ = 0x1e;
		*p++ = 0x19; *p++ = 0x11; *p++ = 0x08;
		/* сколько байт ответа принимаем за раз */
		int want = mtu_sig > 64 ? mtu_sig - 32 : 64;
		if (want > 600) want = 600;
		*p++ = want >> 8; *p++ = want & 0xff;
		/* какие атрибуты: все */
		*p++ = 0x35; *p++ = 0x05;
		*p++ = 0x0a; *p++ = 0x00; *p++ = 0x00; *p++ = 0xff; *p++ = 0xff;
		/* продолжение с прошлого раза */
		*p++ = (unsigned char)cont_len;
		for (int i = 0; i < cont_len; i++)
			*p++ = cont[i];
		int plen_v = (int)(p - req) - 5;
		plen[0] = plen_v >> 8; plen[1] = plen_v & 0xff;

		if (l2_send(sig_dcid, req, (int)(p - req)) < 0) {
			printf("не отправить запрос SDP\n");
			hangup();
			return 1;
		}
		unsigned char rsp[2048];
		int n = l2_recv(sig_scid, rsp, sizeof(rsp), 5000);
		if (n < 7 || rsp[0] != 0x07) {
			printf("устройство не ответило на запрос служб%s\n",
			       n > 0 && rsp[0] == 0x01 ? " (сообщило об ошибке)" : "");
			hangup();
			return 1;
		}
		int alen = (rsp[5] << 8) | rsp[6];
		if (alen < 0 || 7 + alen > n) {
			printf("ответ SDP обрезан\n");
			break;
		}
		if (blob_len + alen > (int)sizeof(blob))
			alen = (int)sizeof(blob) - blob_len;
		memcpy(blob + blob_len, rsp + 7, alen);
		blob_len += alen;

		cont_len = rsp[7 + alen];
		if (cont_len <= 0 || cont_len > 16)
			break;
		memcpy(cont, rsp + 8 + alen, cont_len);
		tid++;
	}

	printf("получено описаний: %d байт\n", blob_len);
	if (!blob_len) {
		printf("устройство не назвало ни одной службы гарнитуры\n");
		hangup();
		return 1;
	}

	/* Разбираем список записей. Внутри каждой ищем имя службы, класс и
	 * номер канала RFCOMM. */
	int type, len, hdr;
	const unsigned char *d = blob;
	int left = blob_len;
	hdr = sdp_hdr(d, left, &type, &len);
	if (hdr < 0 || type != 6) {
		printf("описание служб непонятного вида\n");
		hangup();
		return 1;
	}
	d += hdr; left = len;
	int found = 0;
	while (left > 0) {
		int rl, rt;
		int rh = sdp_hdr(d, left, &rt, &rl);
		if (rh < 0)
			break;
		const unsigned char *rec = d + rh;
		int ch = rfcomm_channel(rec, rl);
		char name[64];
		sdp_service_name(rec, rl, name, sizeof(name));
		int cls = sdp_service_class(rec, rl);
		if (ch > 0) {
			printf("служба %s%s%s: канал RFCOMM %d\n",
			       cls == 0x111e ? "«гарнитура» (Handsfree)" :
			       cls == 0x1108 ? "«наушники» (Headset)" : "неизвестная",
			       name[0] ? ", имя " : "", name[0] ? name : "", ch);
			found++;
		}
		d += rh + rl;
		left -= rh + rl;
	}
	if (!found)
		printf("канал RFCOMM в описании не найден\n");

	hangup();
	return 0;
}
