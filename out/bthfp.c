/* bthfp — шлюз Bluetooth-гарнитуры для звонков с HTC HD2.
 *
 * Телефон не звонит гарнитуре сам: гарнитуры вроде XP500 входящие вызовы
 * почти не обслуживают и подключаются к телефону первыми. Поэтому bthfp
 * ждёт: принимает входящий канал, на опрос служб объявляет себя шлюзом
 * Handsfree и Headset, принимает RFCOMM, ведёт обмен AT-командами
 * профиля и связывает его с демоном модема phoned. Когда идёт разговор,
 * поднимает голосовой канал SCO; сам звук идёт по железной линии PCM
 * между чипом и звуковым процессором, а phoned переключает тракт.
 *
 * Основание (сырой HCI, сопряжение, свой L2CAP) — из bta2dp/btsdp.
 *
 *     bthfp                   BTHFP_DEBUG=1 — шестнадцатеричный обмен
 *
 * Сборка: gcc -O2 bthfp.c -o bthfp -lsbc
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
/* Каналы устройства, по одному на наш из my_scids: в Config Response
 * надо ставить номер канала ПОЛУЧАТЕЛЯ, то есть его, а не наш. */
static unsigned short peer_dcids[4] = { 0, 0, 0, 0 };

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
static void (*ev_hook)(int code, const unsigned char *b, int n) = 0;

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
		if (ev_hook)
			ev_hook(code, b, n);
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
		unsigned short mycid = f[4] | (f[5] << 8);
		int idx = -1;
		for (int i = 0; i < 4; i++)
			if (my_scids[i] && my_scids[i] == mycid) { idx = i; break; }
		if (idx < 0)
			return;
		/* Гарнитура XP500 на ответ с НАШИМ номером канала не реагирует:
		 * сопоставить его со своим каналом она не может, конфигурация с
		 * её стороны не завершается, канал остаётся полуоткрытым и все
		 * запросы молча пропадают. Колонка это прощала. */
		unsigned short their = peer_dcids[idx] ? peer_dcids[idx] : mycid;
		unsigned char r[6] = { their & 0xff, their >> 8, 0, 0, 0, 0 };
		l2_sig(0x05, id, r, 6);
	} else if (code == 0x06 && n >= 8) {        /* Disconnect Request */
		unsigned short dcid = f[4] | (f[5] << 8);
		if (!is_my_cid(dcid))
			return;
		unsigned char r[4] = { f[4], f[5], f[6], f[7] };
		l2_sig(0x07, id, r, 4);
	}
}

/* разбор сигналов гарнитуры (ниже) — нужен и внутри l2_open: пока мы
 * ждём свой канал, гарнитура может просить открыть свои */
static void handle_sig(const unsigned char *f, int n);

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
			for (int i = 0; i < 4; i++)
				if (my_scids[i] == scid)
					peer_dcids[i] = dcid;
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
			handle_sig(f, n);     /* чужие запросы — нашим разбором */
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

/* ══ bthfp: шлюз гарнитуры ═══════════════════════════════════════════ */
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#define HFP_CH   1                  /* канал RFCOMM службы Handsfree AG */
#define HSP_CH   2                  /* канал RFCOMM службы Headset AG   */
#define CID_SDP  0x0070             /* наш канал опроса служб           */
#define CID_RFC  0x0071             /* наш канал RFCOMM                 */

static unsigned short sco_handle = 0;
static int sco_pending = 0;
static double sco_try_at = 0, sco_retry_at = 0;
static int acl_up = 0;
static double acl_at = 0;           /* когда появился канал */
static int init_tried = 0;          /* открывать ли соединение самим */
/* канал найден перебором (без события) — на таком опрос служб обычно
 * срывается, и его лучше порвать и позвать гарнитуру заново */
static int hidden_link = 0;
/* когда гарнитура нажала «ответить»: пока phoned не заметил разговор
 * (опрашивает модем раз в 2 с), сообщаем ей «разговор идёт» сами */
static double ata_at = 0;
/* что мы последним сообщили гарнитуре о звонке */
static int last_call = -1, last_setup = -1;
/* сколько ждать, пока гарнитура откроет соединение сама (BTHFP_INITDELAY) */
static double init_delay = 12;
/* BTHFP_CALL=<MAC>: гарнитура сама не подключается (XP500 так и не
 * постучалась за 15 минут), поэтому зовём её сами, а каналы открывает
 * уже она — это мы и принимаем. */
static unsigned char call_addr[6];
static int call_on = 0;
static double call_next = 0;
/* поиск скрытого канала: контроллер говорит «канал уже есть», а события
 * о его появлении мы не получили — перебираем номера по одному */
static int probe_h = 0;
static unsigned short peer_mtu_of[4] = { 672, 672, 672, 672 };

static void lg(const char *fmt, ...)
{
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	printf("%02d:%02d:%02d ", tm->tm_hour, tm->tm_min, tm->tm_sec);
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static void rd(const char *path, char *b, int n)
{
	b[0] = 0;
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return;
	int r = read(fd, b, n - 1);
	close(fd);
	if (r < 0)
		r = 0;
	b[r] = 0;
	while (r > 0 && (b[r - 1] == '\n' || b[r - 1] == '\r' || b[r - 1] == ' '))
		b[--r] = 0;
}

/* команды демону модема: AT-команды либо служебные «@route b» */
static void phone_cmd(const char *s)
{
	int fd = open("/run/phone/cmd", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		lg("демон модема недоступен — «%s» не отправлено", s);
		return;
	}
	char b[128];
	int n = snprintf(b, sizeof(b), "%s\n", s);
	if (write(fd, b, n) < 0) { }
	close(fd);
}

/* Экран звонка. phoned открывает его сам только для входящего вызова:
 * исходящий показывает интерфейс, который набирал номер. Номер,
 * набранный кнопкой гарнитуры, интерфейс не видит — поэтому экран
 * открываем мы, если он ещё не открыт. */
static void show_callscreen(void)
{
	if (system("pgrep -x callscreen >/dev/null 2>&1 || "
		   "DISPLAY=:0 setsid /usr/local/bin/callscreen "
		   "</dev/null >/dev/null 2>&1 &")) { }
}

static int cid_index(unsigned short cid)
{
	for (int i = 0; i < 4; i++)
		if (my_scids[i] == cid)
			return i;
	return -1;
}

/* забрать из очереди кадр нужного канала, не дожидаясь */
static int take_frame(unsigned short cid, unsigned char *out, int max)
{
	for (int i = 0; i < qn; i++) {
		if (q[i].cid != cid)
			continue;
		int n = q[i].len < max ? q[i].len : max;
		memcpy(out, q[i].data, n);
		memmove(&q[i], &q[i + 1], (qn - i - 1) * sizeof(q[0]));
		qn--;
		return n;
	}
	return 0;
}

static void drop_foreign_frames(void)
{
	for (int i = 0; i < qn;) {
		unsigned short c = q[i].cid;
		if (c == 0x0001 || c == CID_SDP || c == CID_RFC || c == 0x0072) {
			i++;
			continue;
		}
		lg("кадр на неизвестный канал 0x%04x — выбрасываю", c);
		memmove(&q[i], &q[i + 1], (qn - i - 1) * sizeof(q[0]));
		qn--;
	}
}

/* ── голосовой канал ───────────────────────────────────────────────── */
static void on_sco_up(void)
{
	sco_pending = 0;
	lg("голосовой канал поднят (ручка %d)", sco_handle);
	/* тракт уже смотрит на гарнитуру — его ставит служебное соединение */
}

static void on_sco_down(void)
{
	lg("голосовой канал закрыт");
	/* тракт возвращается в динамик только при отключении гарнитуры */
}

static void sco_open(void)
{
	/* Setup Synchronous Connection: 8 кГц, голос 16 бит линейный (его
	 * кодирует в CVSD сам чип), задержка и повторы — на усмотрение
	 * устройства. Пакеты 0x03ff: HV1-3 и EV3-5 без EDR-вариантов —
	 * так соглашаются и старые гарнитуры, и новые. */
	unsigned char p[17] = {
		handle & 0xff, handle >> 8,
		0x40, 0x1f, 0x00, 0x00,
		0x40, 0x1f, 0x00, 0x00,
		0xff, 0xff,
		0x60, 0x00,
		0xff,
		0xff, 0x03
	};
	send_cmd(0x0428, p, 17);
	sco_pending = 1;
	sco_try_at = now_s();
	lg("прошу голосовой канал");
}

static void page_headset(void)
{
	unsigned char cc[13];
	memcpy(cc, call_addr, 6);
	cc[6] = 0x18; cc[7] = 0xcc;            /* DM1..DH5 */
	cc[8] = 0x02;                          /* R2: так до неё дозвалось ядро */
	cc[9] = 0; cc[10] = 0; cc[11] = 0;
	cc[12] = 0x01;                         /* смену роли разрешаем */
	send_cmd(0x0405, cc, 13);
	lg("зову гарнитуру сам (R2), каналы пусть открывает она");
}

/* ── события контроллера (зовётся из pump для каждого события) ─────── */
static void on_event(int code, const unsigned char *b, int n)
{
	if (code == 0x04 && n >= 10) {                   /* Connection Request */
		char m[20];
		mac_str(b, m);
		if (b[9] == 0x01) {
			unsigned char a[7];
			memcpy(a, b, 6);
			a[6] = 0x01;                     /* роль не меняем */
			send_cmd(0x0409, a, 7);
			init_tried = 1;         /* подключилась сама — каналы откроет она */
			lg("входящее подключение от %s — принимаю", m);
		} else {
			unsigned char a[21];
			memcpy(a, b, 6);
			a[6] = 0x40; a[7] = 0x1f; a[8] = 0; a[9] = 0;
			a[10] = 0x40; a[11] = 0x1f; a[12] = 0; a[13] = 0;
			a[14] = 0xff; a[15] = 0xff;
			a[16] = 0x60; a[17] = 0x00;
			a[18] = 0xff;
			a[19] = 0xff; a[20] = 0x03;
			send_cmd(0x0429, a, 21);
			lg("гарнитура сама просит голосовой канал — принимаю");
		}
	} else if (code == 0x03 && n >= 11) {            /* Connection Complete */
		unsigned short h = b[1] | (b[2] << 8);
		if (b[0]) {
			lg("подключение не удалось, код 0x%02x", b[0]);
			return;
		}
		if (b[9] == 0x01) {
			handle = h;
			acl_up = 1;
			char m[20];
			mac_str(b + 3, m);
			lg("канал с %s установлен, ручка %d", m, h);
			acl_at = now_s();
			hidden_link = 0;
			unsigned char lp[4] = { h & 0xff, h >> 8, 0x00, 0x00 };
			send_cmd(0x080d, lp, 4);             /* без дремоты */
			if (call_on && !getenv("BTHFP_NOAUTH")) {
				unsigned char ah[2] = { h & 0xff, h >> 8 };
				send_cmd(0x0411, ah, 2);         /* Authentication Requested */
				lg("прошу аутентификацию");
			}
		} else {
			sco_handle = h;
			on_sco_up();
		}
	} else if (code == 0x2c && n >= 3) {             /* Sync Connection Complete */
		if (b[0]) {
			lg("голосовой канал не поднялся, код 0x%02x", b[0]);
			sco_pending = 0;
			sco_retry_at = now_s() + 5;
			return;
		}
		sco_handle = b[1] | (b[2] << 8);
		on_sco_up();
	} else if (code == 0x05 && n >= 4) {             /* Disconnection */
		unsigned short h = b[1] | (b[2] << 8);
		if (sco_handle && h == sco_handle) {
			sco_handle = 0;
			on_sco_down();
		}
	} else if (code == 0x0f && n >= 4 && b[0] &&
		   b[2] == 0x28 && b[3] == 0x04) {       /* отказ Setup Sync */
		sco_pending = 0;
		sco_retry_at = now_s() + 5;
	} else if (code == 0x06 && n >= 3) {             /* Authentication Complete */
		if (b[0]) {
			lg("аутентификация не прошла, код 0x%02x", b[0]);
		} else {
			lg("аутентификация пройдена — включаю шифрование");
			unsigned char e[3] = { b[1], b[2], 0x01 };
			send_cmd(0x0413, e, 3);
		}
	} else if (code == 0x0f && n >= 4 && b[0] == 0x0b &&
		   b[2] == 0x05 && b[3] == 0x04) {       /* вызов: «канал уже есть» */
		if (!acl_up && !probe_h) {
			lg("канал с гарнитурой уже есть, но его номер неизвестен — ищу");
			probe_h = 1;
			unsigned char rv[2] = { 1, 0 };
			send_cmd(0x041d, rv, 2);         /* Read Remote Version */
		}
	} else if (code == 0x0f && n >= 4 && b[2] == 0x1d && b[3] == 0x04) {
		if (probe_h && b[0]) {                 /* такого номера нет — следующий */
			if (++probe_h > 64) {
				lg("скрытый канал не найден");
				probe_h = 0;
			} else {
				unsigned char rv[2] = { probe_h, 0 };
				send_cmd(0x041d, rv, 2);
			}
		}
	} else if (code == 0x0e && n >= 4 && b[1] == 0x05 && b[2] == 0x14) {
		/* Read RSSI: контроллер не сообщает о разрыве, поэтому канал
		 * проверяем сами — «такого канала нет» значит гарнитура ушла */
		if (b[3] == 0x12 && acl_up && handle) {
			lg("канал с гарнитурой пропал без извещения — считаю отключённой");
			handle = 0;
		}
	} else if (code == 0x0c && n >= 3) {             /* Read Remote Version Complete */
		unsigned short h = b[1] | (b[2] << 8);
		if (probe_h && !acl_up) {
			handle = h;
			acl_up = 1;
			probe_h = 0;
			lg("скрытый канал найден, ручка %d — беру его себе", h);
			acl_at = now_s();
			hidden_link = 1;
			if (call_on && !getenv("BTHFP_NOAUTH")) {
				unsigned char ah[2] = { h & 0xff, h >> 8 };
				send_cmd(0x0411, ah, 2);
				lg("прошу аутентификацию");
			}
			unsigned char lp[4] = { h & 0xff, h >> 8, 0x00, 0x00 };
			send_cmd(0x080d, lp, 4);
		}
	} else if (code == 0x08 && n >= 4) {
		lg("шифрование канала: %s", b[3] ? "включено" : "выключено");
	}
}

/* ── L2CAP: входящие каналы ─────────────────────────────────────────── */
static void rfc_reset(void);

static void handle_sig(const unsigned char *f, int n)
{
	if (n < 4)
		return;
	int code = f[0], id = f[1];
	const unsigned char *p = f + 4;
	int plen = n - 4;
	if (code == 0x02 && plen >= 4) {                 /* Connection Request */
		unsigned short psm = p[0] | (p[1] << 8);
		unsigned short ours = psm == 1 ? CID_SDP : psm == 3 ? CID_RFC : 0;
		if (!ours) {
			unsigned char r[8] = { 0, 0, p[2], p[3], 0x02, 0x00, 0, 0 };
			l2_sig(0x03, id, r, 8);
			lg("просят канал PSM %d — такого у нас нет", psm);
			return;
		}
		int i = cid_index(ours);
		peer_dcids[i] = p[2] | (p[3] << 8);
		peer_mtu_of[i] = 672;
		unsigned char r[8] = { ours & 0xff, ours >> 8, p[2], p[3], 0, 0, 0, 0 };
		l2_sig(0x03, id, r, 8);
		unsigned char c[8] = { p[2], p[3], 0, 0, 0x01, 0x02, 0xa0, 0x02 };
		l2_sig(0x04, sig_id++, c, 8);
		if (ours == CID_RFC)
			rfc_reset();
		lg("открываю канал %s", psm == 1 ? "опроса служб" : "RFCOMM");
	} else if (code == 0x04 && plen >= 4) {          /* Config Request */
		unsigned short dc = p[0] | (p[1] << 8);
		int i = cid_index(dc);
		if (i >= 0)
			for (int k = 4; k + 1 < plen;) {
				int t = p[k] & 0x7f, l = p[k + 1];
				if (t == 0x01 && l == 2 && k + 3 < plen)
					peer_mtu_of[i] = p[k + 2] | (p[k + 3] << 8);
				k += 2 + l;
			}
		l2_handle_peer_sig(f, n);
	} else if (code == 0x06 && plen >= 2) {          /* Disconnect Request */
		l2_handle_peer_sig(f, n);
		unsigned short dc = p[0] | (p[1] << 8);
		if (dc == CID_RFC) {
			rfc_reset();
			lg("гарнитура закрыла RFCOMM");
		}
	} else if (code == 0x0a && plen >= 2) {          /* Information Request */
		int type = p[0] | (p[1] << 8);
		if (type == 2) {
			unsigned char r[8] = { 0x02, 0, 0, 0, 0, 0, 0, 0 };
			l2_sig(0x0b, id, r, 8);
		} else if (type == 3) {
			unsigned char r[12] = { 0x03, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0 };
			l2_sig(0x0b, id, r, 12);
		} else {
			unsigned char r[4] = { p[0], p[1], 0x01, 0x00 };
			l2_sig(0x0b, id, r, 4);
		}
	} else if (code == 0x08) {                       /* Echo Request */
		l2_sig(0x09, id, (unsigned char *)p, plen);
	}
}

/* ── SDP-сервер: объявляем себя шлюзом гарнитуры ─────────────────────── */
struct buf { unsigned char d[1024]; int n; };
static void b1(struct buf *b, int v) { if (b->n < (int)sizeof(b->d)) b->d[b->n++] = v; }
static void b2(struct buf *b, int v) { b1(b, v >> 8); b1(b, v & 0xff); }
static void bu16(struct buf *b, int u) { b1(b, 0x19); b2(b, u); }
static int bdes(struct buf *b) { b1(b, 0x35); b1(b, 0); return b->n - 1; }
static void bdes_end(struct buf *b, int pos) { b->d[pos] = b->n - pos - 1; }

/* Запись 0 — Handsfree Audio Gateway, запись 1 — Headset Audio Gateway */
static const int ATTR_IDS[] = { 0x0000, 0x0001, 0x0004, 0x0005, 0x0009,
				0x0100, 0x0301, 0x0311 };
static const int REC_UUIDS[2][6] = {
	{ 0x111f, 0x1203, 0x0100, 0x0003, 0x1002, 0x111e },
	{ 0x1112, 0x1203, 0x0100, 0x0003, 0x1002, 0x1108 },
};

static void attr_value(int r, int id, struct buf *v)
{
	int a, c;
	switch (id) {
	case 0x0000:
		b1(v, 0x0a); b2(v, 0x0001); b2(v, r);
		break;
	case 0x0001:
		a = bdes(v); bu16(v, r ? 0x1112 : 0x111f); bu16(v, 0x1203);
		bdes_end(v, a);
		break;
	case 0x0004:
		a = bdes(v);
		c = bdes(v); bu16(v, 0x0100); bdes_end(v, c);
		c = bdes(v); bu16(v, 0x0003); b1(v, 0x08); b1(v, r ? HSP_CH : HFP_CH);
		bdes_end(v, c);
		bdes_end(v, a);
		break;
	case 0x0005:
		a = bdes(v); bu16(v, 0x1002); bdes_end(v, a);
		break;
	case 0x0009:
		a = bdes(v);
		c = bdes(v); bu16(v, r ? 0x1108 : 0x111e);
		b1(v, 0x09); b2(v, r ? 0x0102 : 0x0105);
		bdes_end(v, c);
		bdes_end(v, a);
		break;
	case 0x0100: {
		const char *s = r ? "Headset Gateway" : "Voice Gateway";
		b1(v, 0x25); b1(v, (int)strlen(s));
		for (const char *k = s; *k; k++)
			b1(v, *k);
		break;
	}
	case 0x0301:
		if (!r) { b1(v, 0x08); b1(v, 0x01); }
		break;
	case 0x0311:
		if (!r) { b1(v, 0x09); b2(v, 0x0000); }
		break;
	}
}

static int el_hdr(const unsigned char *d, int n, int *type, int *len)
{
	if (n < 1)
		return -1;
	int t = d[0] >> 3, si = d[0] & 7, hdr = 1, l = 0;
	switch (si) {
	case 0: l = t ? 1 : 0; break;
	case 1: l = 2; break;
	case 2: l = 4; break;
	case 3: l = 8; break;
	case 4: l = 16; break;
	case 5: if (n < 2) return -1; l = d[1]; hdr = 2; break;
	case 6: if (n < 3) return -1; l = (d[1] << 8) | d[2]; hdr = 3; break;
	default: return -1;
	}
	if (hdr + l > n)
		return -1;
	*type = t;
	*len = l;
	return hdr;
}

struct filt { int n; int lo[16], hi[16]; };

static int parse_filter(const unsigned char *d, int n, struct filt *f)
{
	int t, l, h = el_hdr(d, n, &t, &l);
	if (h < 0 || t != 6)
		return -1;
	f->n = 0;
	for (int k = h; k < h + l;) {
		int et, el, eh = el_hdr(d + k, h + l - k, &et, &el);
		if (eh < 0)
			return -1;
		if (f->n < 16 && et == 1 && el == 2) {
			f->lo[f->n] = f->hi[f->n] = (d[k + eh] << 8) | d[k + eh + 1];
			f->n++;
		} else if (f->n < 16 && et == 1 && el == 4) {
			f->lo[f->n] = (d[k + eh] << 8) | d[k + eh + 1];
			f->hi[f->n] = (d[k + eh + 2] << 8) | d[k + eh + 3];
			f->n++;
		}
		k += eh + el;
	}
	return h + l;
}

static int parse_uuids(const unsigned char *d, int n, int *u, int *nu)
{
	int t, l, h = el_hdr(d, n, &t, &l);
	if (h < 0 || t != 6)
		return -1;
	*nu = 0;
	for (int k = h; k < h + l;) {
		int et, el, eh = el_hdr(d + k, h + l - k, &et, &el);
		if (eh < 0)
			return -1;
		if (et == 3 && *nu < 12) {
			const unsigned char *v = d + k + eh;
			if (el == 2)
				u[(*nu)++] = (v[0] << 8) | v[1];
			else if (el == 4 || el == 16)
				u[(*nu)++] = (v[2] << 8) | v[3];
		}
		k += eh + el;
	}
	return h + l;
}

static int rec_matches(int r, const int *u, int nu)
{
	for (int i = 0; i < nu; i++) {
		int ok = 0;
		for (int k = 0; k < 6; k++)
			if (REC_UUIDS[r][k] == u[i])
				ok = 1;
		if (!ok)
			return 0;
	}
	return nu > 0;
}

static void attr_list(int r, const struct filt *f, struct buf *out)
{
	int a = bdes(out);
	for (unsigned i = 0; i < sizeof(ATTR_IDS) / sizeof(ATTR_IDS[0]); i++) {
		int id = ATTR_IDS[i], want = 0;
		for (int k = 0; k < f->n; k++)
			if (id >= f->lo[k] && id <= f->hi[k])
				want = 1;
		if (!want)
			continue;
		struct buf v;
		v.n = 0;
		attr_value(r, id, &v);
		if (!v.n)
			continue;
		b1(out, 0x09); b2(out, id);
		for (int k = 0; k < v.n; k++)
			b1(out, v.d[k]);
	}
	bdes_end(out, a);
}

static void sdp_send(const unsigned char *d, int n)
{
	int i = cid_index(CID_SDP);
	if (i >= 0 && peer_dcids[i])
		l2_send(peer_dcids[i], d, n);
}

static void sdp_error(int tid, int code)
{
	unsigned char r[7] = { 0x01, tid >> 8, tid & 0xff, 0x00, 0x02,
			       code >> 8, code & 0xff };
	sdp_send(r, 7);
}

static void sdp_frame(const unsigned char *f, int n)
{
	if (n < 5)
		return;
	int pdu = f[0], tid = (f[1] << 8) | f[2];
	int plen = (f[3] << 8) | f[4];
	const unsigned char *p = f + 5;
	int left = n - 5 < plen ? n - 5 : plen;
	int u[12], nu = 0, pos = 0, maxb = 0, rec = -1, k;
	struct filt flt;
	flt.n = 0;

	if (pdu == 0x02 || pdu == 0x06) {
		if ((k = parse_uuids(p, left, u, &nu)) < 0) { sdp_error(tid, 3); return; }
		pos = k;
		if (pos + 2 > left) { sdp_error(tid, 3); return; }
		maxb = (p[pos] << 8) | p[pos + 1];
		pos += 2;
		if (pdu == 0x06) {
			if ((k = parse_filter(p + pos, left - pos, &flt)) < 0) { sdp_error(tid, 3); return; }
			pos += k;
		}
	} else if (pdu == 0x04) {
		if (left < 6) { sdp_error(tid, 3); return; }
		long hnd = ((long)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
		rec = (hnd == 0x00010000) ? 0 : (hnd == 0x00010001) ? 1 : -1;
		maxb = (p[4] << 8) | p[5];
		pos = 6;
		if ((k = parse_filter(p + pos, left - pos, &flt)) < 0) { sdp_error(tid, 3); return; }
		pos += k;
		if (rec < 0) { sdp_error(tid, 2); return; }
	} else {
		lg("опрос служб: непонятный запрос 0x%02x", pdu);
		sdp_error(tid, 3);
		return;
	}
	int off = 0;
	if (pos < left && p[pos] == 2 && pos + 2 < left)
		off = (p[pos + 1] << 8) | p[pos + 2];

	if (pdu == 0x02) {
		unsigned char r[32];
		int m = 0, cnt = 0;
		r[m++] = 0x03; r[m++] = tid >> 8; r[m++] = tid & 0xff;
		m += 2;
		int cpos = m;
		m += 4;
		for (int i = 0; i < 2; i++)
			if (rec_matches(i, u, nu) && cnt < maxb) {
				r[m++] = 0x00; r[m++] = 0x01; r[m++] = 0x00; r[m++] = i;
				cnt++;
			}
		r[cpos] = 0; r[cpos + 1] = cnt; r[cpos + 2] = 0; r[cpos + 3] = cnt;
		r[m++] = 0;
		r[3] = (m - 5) >> 8; r[4] = (m - 5) & 0xff;
		sdp_send(r, m);
		lg("опрос служб: поиск — найдено записей %d", cnt);
		return;
	}

	struct buf full;
	full.n = 0;
	if (pdu == 0x06) {
		int a = bdes(&full);
		for (int i = 0; i < 2; i++)
			if (rec_matches(i, u, nu))
				attr_list(i, &flt, &full);
		bdes_end(&full, a);
	} else {
		attr_list(rec, &flt, &full);
	}
	int mtu = peer_mtu_of[cid_index(CID_SDP)];
	int room = mtu - 5 - 2 - 3;
	if (maxb < room)
		room = maxb;
	if (room < 1)
		room = 1;
	if (off > full.n)
		off = full.n;
	int chunk = full.n - off;
	if (chunk > room)
		chunk = room;
	int more = off + chunk < full.n;
	unsigned char r[1100];
	int m = 0;
	r[m++] = pdu + 1; r[m++] = tid >> 8; r[m++] = tid & 0xff;
	m += 2;
	r[m++] = chunk >> 8; r[m++] = chunk & 0xff;
	memcpy(r + m, full.d + off, chunk);
	m += chunk;
	if (more) {
		r[m++] = 2; r[m++] = (off + chunk) >> 8; r[m++] = (off + chunk) & 0xff;
	} else
		r[m++] = 0;
	r[3] = (m - 5) >> 8; r[4] = (m - 5) & 0xff;
	sdp_send(r, m);
	lg("опрос служб: ответ %d из %d байт%s", off + chunk, full.n,
	   more ? " (продолжение будет)" : "");
}

/* ── RFCOMM ─────────────────────────────────────────────────────────── */
static unsigned char crctab[256];
static void crc_init(void)
{
	for (int i = 0; i < 256; i++) {
		unsigned char c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ 0xe0 : c >> 1;
		crctab[i] = c;
	}
}
static unsigned char fcs(const unsigned char *d, int n)
{
	unsigned char c = 0xff;
	while (n--)
		c = crctab[c ^ *d++];
	return 0xff - c;
}

static int dlci_up = 0, cfc = 0, tx_cred = 0, rx_cred = 0, rfc_mfs = 127;
static int rfc_init = 0;            /* сеанс RFCOMM открыли мы */
#define CR_CMD (rfc_init ? 1 : 0)    /* наши команды и данные */
#define CR_RSP (rfc_init ? 0 : 1)    /* наши ответы (UA)      */
static int got_ua0 = 0, got_ua_dlci = 0, got_pn = 0;
#define CID_SDPC 0x0072             /* наш канал опроса служб гарнитуры */
static int slc = 0, cmer = 0, clip = 0;
static char at_buf[256];
static int at_len = 0;
static unsigned char outq[2048];
static int outn = 0;

static void rfc_reset(void)
{
	dlci_up = cfc = tx_cred = rx_cred = 0;
	rfc_mfs = 127;
	slc = cmer = clip = 0;
	at_len = outn = 0;
	rfc_init = 0;
	got_ua0 = got_ua_dlci = got_pn = 0;
}

/* Мы — отвечающая сторона сеанса: наши ответы (UA) идут с C/R=1, наши
 * команды и данные — с C/R=0. */
static void rfc_send(int dlci, int cr, int ctrl, const unsigned char *info,
		     int len, int credits)
{
	int i = cid_index(CID_RFC);
	if (i < 0 || !peer_dcids[i])
		return;
	unsigned char fr[512];
	int m = 0;
	fr[m++] = (dlci << 2) | (cr << 1) | 1;
	int uih = (ctrl & ~0x10) == 0xef;
	fr[m++] = credits > 0 ? (ctrl | 0x10) : ctrl;
	if (len < 128)
		fr[m++] = (len << 1) | 1;
	else {
		fr[m++] = (len & 0x7f) << 1;
		fr[m++] = len >> 7;
	}
	unsigned char fv = uih ? fcs(fr, 2) : fcs(fr, m);
	if (credits > 0)
		fr[m++] = credits;
	if (len)
		memcpy(fr + m, info, len);
	m += len;
	fr[m++] = fv;
	l2_send(peer_dcids[i], fr, m);
}

static void mcc_send(int type, int cr, const unsigned char *v, int len)
{
	unsigned char m[64];
	int k = 0;
	m[k++] = (type << 2) | (cr << 1) | 1;
	m[k++] = (len << 1) | 1;
	memcpy(m + k, v, len);
	k += len;
	rfc_send(0, CR_CMD, 0xef, m, k, 0);
}

static void rfc_flush(void)
{
	while (outn > 0 && dlci_up && (!cfc || tx_cred > 0)) {
		int c = outn < rfc_mfs ? outn : rfc_mfs;
		int give = 0;
		if (cfc && rx_cred <= 3) {
			give = 10;
			rx_cred += 10;
		}
		rfc_send(dlci_up, CR_CMD, 0xef, outq, c, give);
		if (cfc)
			tx_cred--;
		memmove(outq, outq + c, outn - c);
		outn -= c;
	}
}

static void rfc_data(const char *d, int n)
{
	if (outn + n > (int)sizeof(outq))
		n = (int)sizeof(outq) - outn;
	memcpy(outq + outn, d, n);
	outn += n;
	rfc_flush();
}

static void at_send(const char *fmt, ...)
{
	char s[256], b[270];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	int n = snprintf(b, sizeof(b), "\r\n%s\r\n", s);
	rfc_data(b, n);
	lg("→ %s", s);
}

/* ── показатели для гарнитуры ─────────────────────────────────────── */
static void ind_values(int *v)
{
	char st[16], csq[8], cap[8];
	rd("/run/phone/state", st, sizeof(st));
	rd("/run/phone/csq", csq, sizeof(csq));
	rd("/sys/class/power_supply/battery/capacity", cap, sizeof(cap));
	v[0] = 1;                                           /* service   */
	v[1] = !strcmp(st, "active");                       /* call      */
	v[2] = !strcmp(st, "ringing") ? 1 : !strcmp(st, "dialing") ? 2 : 0;
	v[3] = 0;                                           /* callheld  */
	int s = atoi(csq);
	v[4] = s < 0 ? 0 : s > 5 ? 5 : s;                   /* signal    */
	v[5] = 0;                                           /* roam      */
	int b = cap[0] ? atoi(cap) / 20 : 5;
	v[6] = b < 0 ? 0 : b > 5 ? 5 : b;                   /* battchg   */
}

static void last_number(char *out, int max)
{
	out[0] = 0;
	FILE *fp = fopen("/var/lib/phone/history", "r");
	if (!fp)
		return;
	char line[256];
	while (fgets(line, sizeof(line), fp)) {
		char *a = strchr(line, ';');
		if (!a || strncmp(a + 1, "out;", 4))
			continue;
		char *num = a + 5, *e = strchr(num, ';');
		if (!e)
			continue;
		*e = 0;
		snprintf(out, max, "%s", num);
	}
	fclose(fp);
}

static void at_line(const char *s)
{
	char u[256];
	int i;
	for (i = 0; s[i] && i < 255; i++)
		u[i] = toupper((unsigned char)s[i]);
	u[i] = 0;
	lg("← %s", s);
	char st[16];
	rd("/run/phone/state", st, sizeof(st));

	if (!strncmp(u, "AT+BRSF", 7)) {
		at_send("+BRSF: 40");   /* 0x20 отклонять вызов, 0x08 звонок по голосовому каналу */
		at_send("OK");
	} else if (!strcmp(u, "AT+CIND=?")) {
		at_send("+CIND: (\"service\",(0,1)),(\"call\",(0,1)),"
			"(\"callsetup\",(0-3)),(\"callheld\",(0-2)),"
			"(\"signal\",(0-5)),(\"roam\",(0,1)),(\"battchg\",(0-5))");
		at_send("OK");
	} else if (!strcmp(u, "AT+CIND?")) {
		int v[7];
		ind_values(v);
		at_send("+CIND: %d,%d,%d,%d,%d,%d,%d",
			v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
		at_send("OK");
	} else if (!strncmp(u, "AT+CMER", 7)) {
		const char *c = strrchr(u, ',');
		cmer = c ? atoi(c + 1) : 0;
		at_send("OK");
		if (cmer && !slc) {
			slc = 1;
			lg("служебное соединение с гарнитурой установлено");
			/* звук разговора — в гарнитуру заранее, чтобы голос сразу
			 * стартовал на ней, без переключения посреди разговора */
			phone_cmd("@route b");
			lg("тракт разговора переключён на гарнитуру");
			{
				int vgs = getenv("BTHFP_VGS") ? atoi(getenv("BTHFP_VGS")) : 13;
				int vgm = getenv("BTHFP_VGM") ? atoi(getenv("BTHFP_VGM")) : 12;
				at_send("+VGS: %d", vgs);
				at_send("+VGM: %d", vgm);
			}
		}
	} else if (!strncmp(u, "AT+CHLD=?", 9)) {
		at_send("+CHLD: (0,1,2,3)");
		at_send("OK");
	} else if (!strncmp(u, "AT+CLIP=", 8)) {
		clip = u[8] == '1';
		at_send("OK");
	} else if (!strcmp(u, "ATA")) {
		if (!strcmp(st, "active") && now_s() - ata_at > 3) {
			/* Гарнитура прислала «ответить» посреди разговора: она считает,
			 * что всё ещё звонят. Это нажатие на отбой. */
			phone_cmd("ATH");
			at_send("OK");
			lg("кнопка посреди разговора — кладу трубку");
		} else {
			phone_cmd("ATA");
			at_send("OK");
			ata_at = now_s();
			/* сразу сообщаем «разговор идёт», не дожидаясь phoned: иначе
			 * следующее нажатие гарнитура отправит как ещё одно «ответить» */
			if (slc && cmer) {
				if (last_call != 1)
					at_send("+CIEV: 2,1");
				if (last_setup != 0)
					at_send("+CIEV: 3,0");
				last_call = 1;
				last_setup = 0;
			}
		}
	} else if (!strcmp(u, "AT+CHUP")) {
		phone_cmd("ATH");
		at_send("OK");
	} else if (!strncmp(u, "ATD", 3)) {
		if (s[3] == '>') {
			at_send("ERROR");
		} else {
			char cmd[64];
			snprintf(cmd, sizeof(cmd), "%s%s", s,
				 strchr(s, ';') ? "" : ";");
			phone_cmd(cmd);
			show_callscreen();
			at_send("OK");
		}
	} else if (!strcmp(u, "AT+BLDN")) {
		char num[48], cmd[64];
		last_number(num, sizeof(num));
		if (num[0]) {
			snprintf(cmd, sizeof(cmd), "ATD%s;", num);
			phone_cmd(cmd);
			show_callscreen();
			at_send("OK");
		} else
			at_send("ERROR");
	} else if (!strncmp(u, "AT+VTS=", 7)) {
		phone_cmd(s);
		at_send("OK");
	} else if (!strncmp(u, "AT+CKPD", 7)) {           /* кнопка HSP */
		if (!strcmp(st, "ringing"))
			phone_cmd("ATA");
		else if (!strcmp(st, "active") || !strcmp(st, "dialing"))
			phone_cmd("ATH");
		at_send("OK");
	} else if (!strncmp(u, "AT+COPS?", 8)) {
		at_send("+COPS: 0,0,\"GSM\"");
		at_send("OK");
	} else if (!strncmp(u, "AT+CLCC", 7)) {
		if (strcmp(st, "idle")) {
			char num[48];
			rd("/run/phone/number", num, sizeof(num));
			int stat = !strcmp(st, "active") ? 0 :
				   !strcmp(st, "dialing") ? 2 : 4;
			at_send("+CLCC: 1,%d,%d,0,0,\"%s\",129",
				stat == 4 ? 1 : 0, stat, num);
		}
		at_send("OK");
	} else if (!strncmp(u, "AT+BVRA", 7) || !strncmp(u, "AT+NREC", 7)) {
		at_send("ERROR");
	} else if (!strncmp(u, "AT+XAPL=", 8)) {
		at_send("+XAPL=iPhone,0");
		at_send("OK");
	} else {
		at_send("OK");               /* остальное принимаем молча */
	}
}

static void at_input(const unsigned char *d, int n)
{
	for (int i = 0; i < n; i++) {
		char c = d[i];
		if (c == '\r' || c == '\n') {
			if (at_len) {
				at_buf[at_len] = 0;
				at_line(at_buf);
				at_len = 0;
			}
		} else if (at_len < (int)sizeof(at_buf) - 1)
			at_buf[at_len++] = c;
	}
}

static void mcc_handle(const unsigned char *info, int ilen)
{
	if (ilen < 2)
		return;
	int t = info[0] >> 2, cr = (info[0] >> 1) & 1;
	int ml = info[1] >> 1;
	const unsigned char *v = info + 2;
	if (ml > ilen - 2)
		ml = ilen - 2;
	if (t == 0x20 && ml >= 8) {                       /* PN */
		if (!cr) {                        /* ответ на наш PN */
			cfc = (v[1] >> 4) == 0x0e;
			tx_cred = cfc ? v[7] : 0;
			rfc_mfs = v[4] | (v[5] << 8);
			got_pn = 1;
			lg("RFCOMM: гарнитура согласовала кадр до %d, кредиты %s",
			   rfc_mfs, cfc ? "да" : "нет");
			return;
		}
		unsigned char r[8];
		memcpy(r, v, 8);
		if ((v[1] >> 4) == 0x0f) {
			cfc = 1;
			r[1] = 0xe0 | (v[1] & 0x0f);
			tx_cred = v[7];
			r[7] = 7;
			rx_cred = 7;
		} else {
			r[1] = v[1] & 0x0f;
			r[7] = 0;
		}
		int mfs = v[4] | (v[5] << 8);
		int lim = peer_mtu_of[cid_index(CID_RFC)] - 6;
		if (mfs > lim) {
			mfs = lim;
			r[4] = mfs & 0xff;
			r[5] = mfs >> 8;
		}
		rfc_mfs = mfs;
		mcc_send(0x20, 0, r, 8);
		lg("RFCOMM: параметры канала %d, кадр до %d, кредиты %s",
		   v[0] >> 1, mfs, cfc ? "да" : "нет");
	} else if (t == 0x38 || t == 0x24 || t == 0x14 || t == 0x08 ||
		   t == 0x28 || t == 0x18) {
		if (cr)
			mcc_send(t, 0, v, ml);        /* MSC, RPN, RLS, Test, FC */
	} else if (cr) {
		unsigned char r[1] = { info[0] };
		mcc_send(0x04, 0, r, 1);          /* не поддерживаем */
	}
}

static void rfc_frame(const unsigned char *f, int n)
{
	if (n < 4)
		return;
	int dlci = f[0] >> 2, ctrl = f[1];
	int len = f[2] >> 1, k = 3;
	if (!(f[2] & 1)) {
		len |= f[3] << 7;
		k = 4;
	}
	int pf = ctrl & 0x10, type = ctrl & ~0x10;
	if (type == 0x2f) {                                /* SABM */
		rfc_send(dlci, CR_RSP, 0x73, NULL, 0, 0);       /* UA с битом F */
		if (dlci == 0) {
			lg("RFCOMM: сеанс открыт");
		} else {
			dlci_up = dlci;
			lg("RFCOMM: открыт канал %d (%s)", dlci >> 1,
			   dlci == HFP_CH * 2 ? "Handsfree" :
			   dlci == HSP_CH * 2 ? "Headset" : "неизвестный");
			unsigned char msc[2] = { (dlci << 2) | 2 | 1, 0x8d };
			mcc_send(0x38, 1, msc, 2);
		}
	} else if (type == 0x43) {                         /* DISC */
		rfc_send(dlci, CR_RSP, 0x73, NULL, 0, 0);
		if (dlci == 0 || dlci == dlci_up) {
			lg("RFCOMM: гарнитура закрыла %s", dlci ? "канал" : "сеанс");
			if (slc) {
				phone_cmd("@route l");
				lg("тракт разговора возвращён на громкую связь");
			}
			rfc_reset();
		}
	} else if (type == 0x63) {                         /* UA */
		if (dlci == 0) {
			got_ua0 = 1;
		} else {
			dlci_up = dlci;
			got_ua_dlci = 1;
			unsigned char msc[2] = { (dlci << 2) | 2 | 1, 0x8d };
			mcc_send(0x38, 1, msc, 2);
		}
	} else if (type == 0x0f) {                         /* DM */
		lg("RFCOMM: гарнитура отказала в канале %d", dlci >> 1);
	} else if (type == 0xef) {                         /* UIH */
		const unsigned char *info = f + k;
		int avail = n - k - 1;
		if (dlci && pf && cfc && avail > 0) {
			tx_cred += info[0];
			info++;
			avail--;
		}
		if (len > avail)
			len = avail < 0 ? 0 : avail;
		if (dlci == 0)
			mcc_handle(info, len);
		else if (len > 0) {
			if (cfc) {
				rx_cred--;
				if (rx_cred <= 3) {
					rfc_send(dlci, CR_CMD, 0xef, NULL, 0, 10);
					rx_cred += 10;
				}
			}
			at_input(info, len);
		}
		rfc_flush();
	}
}


/* ── разбор входящих кадров (общий для главного цикла и ожиданий) ── */
static void dispatch_frames(void)
{
	unsigned char f[1100];
	int n;
	while ((n = take_frame(0x0001, f, sizeof(f))) > 0)
		handle_sig(f, n);
	while ((n = take_frame(CID_SDP, f, sizeof(f))) > 0)
		sdp_frame(f, n);
	while ((n = take_frame(CID_RFC, f, sizeof(f))) > 0)
		rfc_frame(f, n);
	while ((n = take_frame(CID_SDPC, f, sizeof(f))) > 0)
		;                                /* запоздалые ответы опроса */
	drop_foreign_frames();
}

static int wait_flag(int *flag, double secs)
{
	double end = now_s() + secs;
	while (!*flag && handle && now_s() < end) {
		pump(100);
		dispatch_frames();
	}
	return *flag;
}

/* номер канала RFCOMM службы uuid у гарнитуры; 0 — нет, -1 — молчит */
static int sdp_find_channel(unsigned short dcid, int uuid)
{
	static unsigned char blob[2048];
	int bl = 0, cl = 0;
	unsigned char cont[17];
	for (int round = 0; round < 20; round++) {
		unsigned char rq[48];
		int m = 0;
		rq[m++] = 0x06; rq[m++] = 0; rq[m++] = round + 1;
		m += 2;
		rq[m++] = 0x35; rq[m++] = 0x03;
		rq[m++] = 0x19; rq[m++] = uuid >> 8; rq[m++] = uuid & 0xff;
		rq[m++] = 0x00; rq[m++] = 0x40;          /* до 64 байт за раз */
		rq[m++] = 0x35; rq[m++] = 0x05;          /* только атрибут 0x0004 */
		rq[m++] = 0x0a; rq[m++] = 0x00; rq[m++] = 0x04;
		rq[m++] = 0x00; rq[m++] = 0x04;
		rq[m++] = cl;
		memcpy(rq + m, cont, cl);
		m += cl;
		rq[3] = (m - 5) >> 8; rq[4] = (m - 5) & 0xff;
		l2_send(dcid, rq, m);
		unsigned char r[512];
		int n = l2_recv(CID_SDPC, r, sizeof(r), 8000);
		if (n <= 0) {
			lg("опрос служб гарнитуры: молчит");
			return -1;
		}
		if (r[0] != 0x07 || n < 7) {
			lg("опрос служб гарнитуры: ответ 0x%02x, %d байт", r[0], n);
			return -1;
		}
		int al = (r[5] << 8) | r[6];
		if (8 + al > n)
			return -1;
		if (bl + al <= (int)sizeof(blob)) {
			memcpy(blob + bl, r + 7, al);
			bl += al;
		}
		cl = r[7 + al];
		if (cl <= 0 || cl > 16 || 8 + al + cl > n)
			break;
		memcpy(cont, r + 8 + al, cl);
	}
	for (int i = 0; i + 4 < bl; i++)
		if (blob[i] == 0x19 && blob[i + 1] == 0x00 && blob[i + 2] == 0x03 &&
		    blob[i + 3] == 0x08)
			return blob[i + 4];
	return 0;
}

/* Гарнитура приняла наш вызов, но каналы не открывает — открываем сами
 * (служебное соединение по инициативе шлюза профиль разрешает). */
static void ag_initiate(void)
{
	init_tried = 1;
	lg("гарнитура каналы не открывает — открываю сам");
	int mtu;
	if (peer_dcids[cid_index(CID_RFC)] || dlci_up) {
		lg("гарнитура открыла соединение сама — не мешаю");
		return;
	}
	unsigned short sd = l2_open(1, CID_SDPC, &mtu);
	if (!sd) {
		lg("канал опроса служб гарнитуры не открылся");
		return;
	}
	int hsp = 0, ch = sdp_find_channel(sd, 0x111e);
	if (ch == 0) {
		ch = sdp_find_channel(sd, 0x1108);
		hsp = 1;
	}
	unsigned char dr[4] = { sd & 0xff, sd >> 8, CID_SDPC & 0xff, CID_SDPC >> 8 };
	l2_sig(0x06, sig_id++, dr, 4);
	if (ch <= 0) {
		lg("у гарнитуры не нашлось служб Handsfree и Headset");
		return;
	}
	lg("у гарнитуры служба %s на канале %d", hsp ? "Headset" : "Handsfree", ch);
	if (peer_dcids[cid_index(CID_RFC)] || dlci_up) {
		lg("гарнитура открыла соединение сама — не мешаю");
		return;
	}
	unsigned short rc = l2_open(3, CID_RFC, &mtu);
	if (!rc) {
		lg("канал RFCOMM к гарнитуре не открылся");
		return;
	}
	peer_mtu_of[cid_index(CID_RFC)] = mtu;
	rfc_reset();
	rfc_init = 1;
	rfc_send(0, 1, 0x3f, NULL, 0, 0);                /* SABM сеанса */
	if (!wait_flag(&got_ua0, 10)) {
		lg("RFCOMM: сеанс не открылся");
		return;
	}
	int dl = ch * 2;
	int mfs = mtu - 6;
	if (mfs > 127)
		mfs = 127;
	unsigned char pn[8] = { dl, 0xf0, 0x07, 0x00, mfs & 0xff, mfs >> 8, 0x00, 0x07 };
	rx_cred = 7;
	mcc_send(0x20, 1, pn, 8);
	wait_flag(&got_pn, 10);
	rfc_send(dl, 1, 0x3f, NULL, 0, 0);               /* SABM канала */
	if (!wait_flag(&got_ua_dlci, 10)) {
		lg("RFCOMM: канал %d не открылся", ch);
		return;
	}
	lg("RFCOMM открыт шлюзом — жду AT-команды гарнитуры");
}

/* ── раз в полсекунды: состояние звонка ───────────────────────────── */
static void tick(double t)
{
	static int last_sig = -1, last_bat = -1;
	static double last_ring = 0;
	int v[7];
	ind_values(v);
	if (t - ata_at < 8 && v[2] == 1) {   /* ответ нажат, модем ещё звонит */
		v[1] = 1;
		v[2] = 0;
	}
	int report = dlci_up && slc && cmer;
	if (report) {
		if (last_call >= 0 && v[1] != last_call)
			at_send("+CIEV: 2,%d", v[1]);
		if (last_setup >= 0 && v[2] != last_setup)
			at_send("+CIEV: 3,%d", v[2]);
		if (last_sig >= 0 && v[4] != last_sig)
			at_send("+CIEV: 5,%d", v[4]);
		if (last_bat >= 0 && v[6] != last_bat)
			at_send("+CIEV: 7,%d", v[6]);
	}
	last_call = v[1];
	last_setup = v[2];
	last_sig = v[4];
	last_bat = v[6];

	if (dlci_up && v[2] == 1 && t - last_ring >= 3) {
		last_ring = t;
		at_send("RING");
		if (clip) {
			char num[48];
			rd("/run/phone/number", num, sizeof(num));
			if (num[0])
				at_send("+CLIP: \"%s\",129", num);
		}
	}

	/* голосовой канал нужен и пока звонит: мелодия идёт в гарнитуру по нему */
	int want = dlci_up && handle && (v[1] || v[2] == 1 || v[2] == 2);
	if (want && !sco_handle && !sco_pending && t >= sco_retry_at)
		sco_open();
	if (sco_pending && t - sco_try_at > 8) {
		lg("голосовой канал не ответил — попробую ещё");
		sco_pending = 0;
		sco_retry_at = t + 5;
	}
	if (!want && sco_handle) {
		unsigned char dc[3] = { sco_handle & 0xff, sco_handle >> 8, 0x13 };
		send_cmd(0x0406, dc, 3);
		lg("звонок окончен — закрываю голосовой канал");
	}
}

static int hex_list(const char *s, unsigned char *out, int max)
{
	int n = 0;
	while (s && *s && n < max) {
		char *e;
		long v = strtol(s, &e, 16);
		if (e == s)
			break;
		out[n++] = (unsigned char)v;
		s = e;
	}
	return n;
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	debug = getenv("BTHFP_DEBUG") != NULL;
	crc_init();
	if (system("pkill -x btagent >/dev/null 2>&1")) { }

	hci = socket(31, SOCK_RAW, 1);
	if (hci < 0) {
		perror("socket");
		return 1;
	}
	struct { unsigned short family, dev, channel; } sa = { 31, 0, 0 };
	if (bind(hci, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind");
		return 1;
	}
	struct { uint32_t type_mask; uint32_t event_mask[2]; uint16_t opcode; } flt;
	memset(&flt, 0, sizeof(flt));
	flt.type_mask = (1 << 2) | (1 << 4);
	flt.event_mask[0] = flt.event_mask[1] = 0xffffffff;
	if (setsockopt(hci, 0, 2, &flt, sizeof(flt)) < 0)
		perror("фильтр HCI");
	raw_mode(1);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	ev_hook = on_event;
	my_scids[2] = 0x0072;               /* наш канал опроса служб гарнитуры */
	if (getenv("BTHFP_INITDELAY"))
		init_delay = atof(getenv("BTHFP_INITDELAY"));
	{
		const char *ca = getenv("BTHFP_CALL");
		unsigned v[6];
		if (ca && sscanf(ca, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2],
					  &v[3], &v[4], &v[5]) == 6) {
			for (int i = 0; i < 6; i++)
				call_addr[i] = (unsigned char)v[5 - i];
			call_on = 1;
		}
	}

	/* виден и принимает подключения; класс — телефон с голосом */
	unsigned char scan = 0x03;
	send_cmd(0x0c1a, &scan, 1);
	unsigned char cod[3] = { 0x04, 0x02, 0x5a };
	send_cmd(0x0c24, cod, 3);
	/* голос: 16 бит линейный, в CVSD кодирует чип */
	unsigned char vs[2] = { 0x60, 0x00 };
	send_cmd(0x0c26, vs, 2);
	/* Голос по железной линии PCM. На стороне процессора выводы SYNC и
	 * CLK настроены входами — значит ведущий на линии чип Bluetooth:
	 * маршрут PCM, 128 кбит/с, короткая синхронизация, чип ведущий по
	 * синхронизации и по такту. Переопределяется BTHFP_PCM="00 00 00 00 00". */
	unsigned char pcm[5] = { 0x00, 0x00, 0x00, 0x00, 0x00 };
	hex_list(getenv("BTHFP_PCM"), pcm, 5);
	send_cmd(0xfc1c, pcm, 5);
	{
		unsigned char fmt[5];
		if (hex_list(getenv("BTHFP_PCMFMT"), fmt, 5) == 5) {
			send_cmd(0xfc1e, fmt, 5);
			lg("формат PCM у чипа: %02x %02x %02x %02x %02x",
			   fmt[0], fmt[1], fmt[2], fmt[3], fmt[4]);
		}
	}
	lg("линия PCM у чипа: %02x %02x %02x %02x %02x",
	   pcm[0], pcm[1], pcm[2], pcm[3], pcm[4]);
	for (int i = 0; i < 10; i++)
		pump(50);

	lg("шлюз гарнитуры запущен: телефон виден, службы Handsfree (канал %d) "
	   "и Headset (канал %d) объявлены — включите гарнитуру", HFP_CH, HSP_CH);

	double last_tick = 0;
	for (;;) {
		pump(100);
		dispatch_frames();
		if (acl_up && !handle) {
			acl_up = 0;
			rfc_reset();
			for (int i = 0; i < 4; i++)
				peer_dcids[i] = 0;
			if (sco_handle) {
				sco_handle = 0;
				on_sco_down();
			}
			sco_pending = 0;
			init_tried = 0;
			hidden_link = 0;
			last_call = last_setup = -1;
			lg("гарнитура отключилась — жду снова");
			phone_cmd("@route l");
			lg("тракт разговора возвращён на громкую связь");
			send_cmd(0x0c1a, &scan, 1);
		}
		double t = now_s();
		{
			static double last_alive = 0;
			if (acl_up && handle && t - last_alive >= 5) {
				last_alive = t;
				unsigned char rh[2] = { handle & 0xff, handle >> 8 };
				send_cmd(0x1405, rh, 2);         /* Read RSSI */
			}
		}
		/* На скрытом канале гарнитура сама соединение не открывает —
		 * ждать ей 12 с незачем. */
		if (acl_up && call_on && !init_tried && !dlci_up &&
		    t - acl_at > (hidden_link ? 3 : init_delay)) {
			ag_initiate();
			if (!dlci_up && hidden_link && handle) {
				/* на скрытом канале не вышло — рвём его и сразу зовём
				 * заново: следующий вызов обычно даёт нормальный канал */
				unsigned char dc[3] = { handle & 0xff, handle >> 8, 0x13 };
				send_cmd(0x0406, dc, 3);
				lg("скрытый канал не годится — рву и зову гарнитуру заново");
				handle = 0;
				call_next = now_s() + 2;
			}
		}
		if (call_on && !acl_up && t >= call_next) {
			page_headset();
			call_next = t + 7;    /* срок ответа на вызов 5.12 с — чаще не надо */
		}
		if (t - last_tick >= 0.5) {
			last_tick = t;
			tick(t);
		}
	}
}
