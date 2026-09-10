/* bta2dp — звук на Bluetooth-колонку с HTC HD2 без bluetoothd и без
 * L2CAP ядра.
 *
 * ПОЧЕМУ МИМО ЯДРА. Колонка JBL отвечает на страничный вызов только в
 * режиме повторения R0 (проверено 05.09: R0 — соединение сразу, R1 и
 * R2 — молчание). Ядро 3.0 зовёт в R2 либо в режиме из кэша поиска, а
 * чужое соединение, установленное не им, игнорирует
 * (hci_conn_complete_evt: «goto unlock»). Поэтому весь путь наш:
 *
 *   сырой HCI-сокет → Create Connection (R0) → ACL-канал
 *     → L2CAP (CID 1: Connect/Config) к PSM 25 — канал сигнализации
 *     → AVDTP: DISCOVER, GET_CAPABILITIES, SET_CONFIGURATION, OPEN
 *     → второй L2CAP-канал к PSM 25 — канал данных
 *     → START → RTP-пакеты с SBC-кадрами по часам.
 *
 * Сопряжение (Link Key / PIN / SSP) обрабатываем здесь же, поэтому на
 * время работы btagent надо остановить — иначе ответы задвоятся.
 *
 *     bta2dp <MAC>             — разведка (до GET_CAPABILITIES)
 *     bta2dp <MAC> файл.wav    — играть (WAV 44100/стерео/16 бит)
 *     bta2dp <MAC> -           — PCM того же формата со stdin
 *
 * Сборка: gcc -O2 bta2dp.c -lsbc -o bta2dp
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

static int avdtp_cmd(int signal, const unsigned char *pl, int n,
		     unsigned char *rsp, int rmax)
{
	unsigned char pkt[300];
	label = (label + 1) & 0x0f;
	pkt[0] = (label << 4);                       /* single, command */
	pkt[1] = signal;
	if (n)
		memcpy(pkt + 2, pl, n);
	if (l2_send(sig_dcid, pkt, 2 + n) < 0)
		return -1;
	double end = now_s() + 5;
	while (now_s() < end) {
		unsigned char d[700];
		int r = l2_recv(sig_scid, d, sizeof(d), 500);
		if (r < 2)
			continue;
		int lbl = d[0] >> 4, type = d[0] & 3;
		if (lbl != label || d[1] != signal)
			continue;
		if (type == 2) {
			int body = r - 2;
			if (body > rmax)
				body = rmax;
			memcpy(rsp, d + 2, body);
			return body;
		}
		if (type == 3) {
			printf("сигнал %d отвергнут, код 0x%02x\n", signal,
			       r > 2 ? d[r - 1] : 0);
			return -1;
		}
	}
	printf("нет ответа на сигнал %d\n", signal);
	return -1;
}

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
		} else if (d[1] == 0x0f && r >= 7 && b[0] &&
			   b[2] == 0x05 && b[3] == 0x04) {
			/* Статус ИМЕННО вызова (0x0405). Статусы наших же разрывов
			 * при самолечении («нет такой ручки», 0x12) сюда не
			 * попадают — раньше они принимались за отказ на вызов. */
			if (b[0] == 0x0b && !retried) {
				/* «канал уже есть» — остался от убитого процесса;
				 * рвём возможные ручки и зовём ещё раз */
				say("канал уже есть — рву старый и зову снова");
				for (int h = 0x0b; h <= 0x0e; h++) {
					unsigned char dc[3] = { (unsigned char)h, 0, 0x13 };
					send_cmd(0x0406, dc, 3);
					usleep(150000);
				}
				/* выгребаем ответы на разрывы, чтобы они не
				 * перепутались с ответом на новый вызов */
				{
					double t_end = now_s() + 0.6;
					while (now_s() < t_end) {
						struct pollfd pf2 = { hci, POLLIN, 0 };
						if (poll(&pf2, 1, 100) > 0) {
							unsigned char junk[300];
							if (read(hci, junk, sizeof(junk)) < 0)
								break;
						}
					}
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

	/* 2. канал сигнализации */
	int mtu_sig;
	sig_dcid = l2_open(AVDTP_PSM, sig_scid, &mtu_sig);
	if (!sig_dcid) {
		hangup();
		return 1;
	}
	say("канал сигнализации открыт");

	/* 3. DISCOVER */
	unsigned char rsp[300];
	int n = avdtp_cmd(DISCOVER, NULL, 0, rsp, sizeof(rsp));
	if (n < 2) {
		say("колонка не ответила на DISCOVER");
		hangup();
		return 1;
	}
	int seid = -1;
	for (int i = 0; i + 1 < n; i += 2) {
		int id = rsp[i] >> 2, inuse = (rsp[i] >> 1) & 1;
		int media = rsp[i + 1] >> 4, tsep = (rsp[i + 1] >> 3) & 1;
		printf("SEP %d: %s%s%s\n", id, media == 0 ? "звук" : "не звук",
		       tsep ? ", приёмник" : ", источник", inuse ? ", занят" : "");
		if (seid < 0 && media == 0 && tsep && !inuse)
			seid = id;
	}
	if (seid < 0) {
		say("свободного звукового приёмника нет");
		hangup();
		return 1;
	}

	/* 4. GET_CAPABILITIES */
	unsigned char qs[1] = { (unsigned char)(seid << 2) };
	n = avdtp_cmd(GET_CAPS, qs, 1, rsp, sizeof(rsp));
	if (n < 0) {
		hangup();
		return 1;
	}
	int have_sbc = 0, c0 = 0, c1 = 0, bpmin = 2, bpmax = 53;
	for (int i = 0; i + 1 < n;) {
		int cat = rsp[i], len = rsp[i + 1];
		if (cat == 7 && len >= 6 && rsp[i + 3] == 0) {
			have_sbc = 1;
			c0 = rsp[i + 4]; c1 = rsp[i + 5];
			bpmin = rsp[i + 6]; bpmax = rsp[i + 7];
		}
		i += 2 + len;
	}
	if (!have_sbc) {
		printf("SEP %d не умеет SBC\n", seid);
		hangup();
		return 1;
	}
	printf("SBC у SEP %d: частоты%s%s%s%s, режимы%s%s%s%s, "
	       "блоки/подполосы 0x%02x, битпул %d..%d\n", seid,
	       c0 & 0x80 ? " 16k" : "", c0 & 0x40 ? " 32k" : "",
	       c0 & 0x20 ? " 44.1k" : "", c0 & 0x10 ? " 48k" : "",
	       c0 & 8 ? " моно" : "", c0 & 4 ? " dual" : "",
	       c0 & 2 ? " стерео" : "", c0 & 1 ? " joint" : "", c1, bpmin,
	       bpmax);
	fflush(stdout);
	if (argc < 3) {
		say("разведка закончена");
		hangup();
		return 0;
	}

	/* 5. SET_CONFIGURATION */
	if (!(c0 & 0x20)) {
		say("колонка не умеет 44100");
		hangup();
		return 1;
	}
	int mode = (c0 & 1) ? 1 : (c0 & 2) ? 2 : 0;
	if (!mode) {
		say("колонка не умеет стерео");
		hangup();
		return 1;
	}
	int bp = bpmax < 53 ? bpmax : 53;
	if (bp < bpmin)
		bp = bpmin;
	unsigned char conf[] = {
		(unsigned char)(seid << 2), (unsigned char)(1 << 2),
		1, 0,
		7, 6, 0x00, 0x00, (unsigned char)(0x20 | mode), 0x15,
		(unsigned char)bp, (unsigned char)bp
	};
	if (avdtp_cmd(SET_CONF, conf, sizeof(conf), rsp, sizeof(rsp)) < 0) {
		hangup();
		return 1;
	}
	printf("конфигурация принята: 44100, %s, битпул %d\n",
	       mode == 1 ? "joint stereo" : "stereo", bp);

	/* 6. OPEN и канал данных */
	if (avdtp_cmd(OPEN, qs, 1, rsp, sizeof(rsp)) < 0) {
		hangup();
		return 1;
	}
	int mmtu;
	unsigned short media_scid = 0x0071;
	unsigned short media_dcid = l2_open(AVDTP_PSM, media_scid, &mmtu);
	if (!media_dcid) {
		hangup();
		return 1;
	}
	if (mmtu > acl_mtu - 4)
		mmtu = acl_mtu - 4;
	printf("канал данных открыт, MTU %d\n", mmtu);

	/* 7. START */
	if (avdtp_cmd(START, qs, 1, rsp, sizeof(rsp)) < 0) {
		hangup();
		return 1;
	}
	say("поток запущен");

	/* кодек и источник */
	sbc_t sbc;
	sbc_init(&sbc, 0);
	sbc.frequency = SBC_FREQ_44100;
	sbc.blocks = SBC_BLK_16;
	sbc.subbands = SBC_SB_8;
	sbc.mode = mode == 1 ? SBC_MODE_JOINT_STEREO : SBC_MODE_STEREO;
	sbc.allocation = SBC_AM_LOUDNESS;
	sbc.bitpool = bp;
	sbc.endian = SBC_LE;
	size_t codesize = sbc_get_codesize(&sbc);
	size_t framelen = sbc_get_frame_length(&sbc);
	int fpp = (mmtu - 13) / framelen;
	if (fpp < 1) fpp = 1;
	if (fpp > 15) fpp = 15;
	printf("кадр SBC: %zu байт PCM -> %zu байт, в пакете %d кадров\n",
	       codesize, framelen, fpp);
	fflush(stdout);

	FILE *in;
	if (!strcmp(argv[2], "-"))
		in = stdin;
	else if (!strncmp(argv[2], "/run/", 5) || strstr(argv[2], ".pcm")) {
		/* Именованный канал от плеера. Открываем на ЧТЕНИЕ-ЗАПИСЬ: так
		 * open не ждёт писателя, а чтение никогда не упирается в конец
		 * потока, когда плеер молчит (пауза, смена дорожки) — иначе
		 * bta2dp завершался бы на каждой паузе и рвал соединение. */
		in = fdopen(fifo_fd, "rb");
		printf("читаю канал %s (PCM 44100/стерео/16)%c", argv[2], 10);
	} else {
		in = fopen(argv[2], "rb");
		if (!in) {
			printf("нет файла %s\n", argv[2]);
			hangup();
			return 1;
		}
		unsigned char hdr[44];
		if (fread(hdr, 1, 44, in) != 44 || memcmp(hdr, "RIFF", 4)) {
			say("это не WAV");
			hangup();
			return 1;
		}
		int ch = hdr[22] | (hdr[23] << 8);
		int rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16);
		int bits = hdr[34] | (hdr[35] << 8);
		if (ch != 2 || rate != 44100 || bits != 16) {
			printf("нужен WAV 44100/стерео/16, а тут %d/%d/%d\n", rate,
			       ch, bits);
			hangup();
			return 1;
		}
	}

	unsigned char *pcm = malloc(codesize * fpp);
	unsigned char *pkt = malloc(mmtu);
	unsigned short seq = 0;
	unsigned ts = 0;
	unsigned long frames = 0;
	double t0 = now_s();
	for (;;) {
		double before = now_s();
		size_t got = fread(pcm, 1, codesize * fpp, in);
		if (got < codesize)
			break;
		/* Плеер молчал (пауза, смена дорожки) — начинаем отсчёт заново,
		 * иначе накопленное «опоздание» выплюнется одним залпом. */
		if (now_s() - before > 0.5) {
			t0 = now_s();
			frames = 0;
		}
		int nf = got / codesize;
		pkt[0] = 0x80; pkt[1] = 0x60;
		pkt[2] = seq >> 8; pkt[3] = seq & 0xff;
		pkt[4] = ts >> 24; pkt[5] = ts >> 16; pkt[6] = ts >> 8; pkt[7] = ts;
		pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;
		pkt[12] = nf;
		int off = 13;
		int bad = 0;
		for (int i = 0; i < nf; i++) {
			ssize_t w = 0;
			if (sbc_encode(&sbc, pcm + i * codesize, codesize,
				       pkt + off, mmtu - off, &w) < 0) {
				say("ошибка кодера");
				bad = 1;
				break;
			}
			off += w;
		}
		if (bad)
			break;
		/* BTA2DP_FLOOD — измерительный режим: шлём без пауз, чтобы
		 * увидеть предел радио, а не наш темп. */
		double due = t0 + (double)frames * 128 / 44100.0;
		double wait = getenv("BTA2DP_FLOOD") ? -1 : due - now_s();
		if (wait > 0)
			usleep((useconds_t)(wait * 1e6));
		else
			pump(0);                             /* хоть кредиты забрать */
		if (!handle || l2_send(media_dcid, pkt, off) < 0)
			break;
		seq++;
		ts += nf * 128;
		frames += nf;
		if (frames % 2000 < (unsigned)nf) {
			printf("передано %.1f с\n", frames * 128 / 44100.0);
			fflush(stdout);
		}
	}
	printf("пакетов %ld, байт %ld, ждали кредитов %.1f с\n",
	       n_pkt, n_byte, t_wait);
	printf("конец потока, %.1f с\n", frames * 128 / 44100.0);
	sbc_finish(&sbc);
	if (handle) {
		avdtp_cmd(SUSPEND, qs, 1, rsp, sizeof(rsp));
		avdtp_cmd(CLOSE, qs, 1, rsp, sizeof(rsp));
	}
	hangup();
	return 0;
}
