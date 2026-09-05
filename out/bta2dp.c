/* bta2dp — звук на Bluetooth-колонку с HTC HD2 без bluetoothd.
 *
 * На ядре 3.0 у BlueZ нет интерфейса mgmt, bluetoothd не работает, и
 * профиль A2DP приходится вести самим: L2CAP-сокеты даёт ядро, кодек
 * SBC — libsbc, а сигнализацию AVDTP делаем здесь. Минимальный
 * источник (SRC) по спецификации AVDTP 1.3 / A2DP 1.3:
 *
 *   1. L2CAP к PSM 25 — канал сигнализации;
 *   2. DISCOVER          — какие приёмники (SEP) есть у колонки;
 *   3. GET_CAPABILITIES  — что умеет SBC у выбранного SEP;
 *   4. SET_CONFIGURATION — 44100, joint stereo, 16 блоков, 8 подполос,
 *                          loudness, битпул по возможностям колонки;
 *   5. OPEN              — и второй L2CAP к PSM 25: канал данных;
 *   6. START             — и дальше RTP-пакеты с SBC-кадрами по часам.
 *
 *     bta2dp <MAC>                — только разведка (шаги 1–3)
 *     bta2dp <MAC> файл.wav       — играть (44100, стерео, 16 бит)
 *     bta2dp <MAC> -              — PCM с stdin того же формата
 *
 * Сборка: gcc -O2 bta2dp.c -lsbc -o bta2dp
 */
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sbc/sbc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define AF_BLUETOOTH_  31
#define BTPROTO_L2CAP_ 0
#define SOL_L2CAP_     6
#define L2CAP_OPTIONS_ 0x01
#define AVDTP_PSM      25

struct sockaddr_l2_ {
	unsigned short family;
	unsigned short psm;             /* little-endian */
	unsigned char  bdaddr[6];
	unsigned short cid;
};

struct l2cap_options_ {
	unsigned short omtu, imtu, flush_to;
	unsigned char  mode, fcs, max_tx;
	unsigned short txwin_size;
};

/* сигналы AVDTP */
enum { DISCOVER = 1, GET_CAPS = 2, SET_CONF = 3, OPEN = 6, START = 7,
       CLOSE = 8, SUSPEND = 9, ABORT = 10 };

static unsigned char dst[6];
static int sig_fd = -1, media_fd = -1;
static unsigned char label = 0;

static double now_s(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static int parse_mac(const char *s, unsigned char *out)
{
	unsigned v[6];
	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4],
		   &v[5]) != 6)
		return 0;
	for (int i = 0; i < 6; i++)
		out[i] = (unsigned char)v[5 - i];   /* ядро ждёт задом наперёд */
	return 1;
}

static int l2_connect(unsigned short psm, int *mtu)
{
	int s = socket(AF_BLUETOOTH_, SOCK_SEQPACKET, BTPROTO_L2CAP_);
	if (s < 0) {
		printf("нет L2CAP-сокетов в ядре\n");
		return -1;
	}
	struct sockaddr_l2_ a;
	memset(&a, 0, sizeof(a));
	a.family = AF_BLUETOOTH_;
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		printf("bind: %s\n", strerror(errno));
		close(s);
		return -1;
	}
	memset(&a, 0, sizeof(a));
	a.family = AF_BLUETOOTH_;
	a.psm = psm;
	memcpy(a.bdaddr, dst, 6);
	/* ПРОВЕРЕНО 05.09: у L2CAP-сокетов этого ядра таймаут отправки
	 * нулевой, и connect() возвращает EINPROGRESS сразу, не дождавшись
	 * ACL-канала — так падают даже l2ping и sdptool. А закрытие сокета
	 * отменяет вызов, оставляя в таблице канал в состоянии 5. Задаём
	 * таймаут явно — тогда connect() честно ждёт установки канала. */
	struct timeval sto = {40, 0};
	int rc = setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof(sto));
	struct timeval chk = {0, 0};
	socklen_t cl = sizeof(chk);
	getsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &chk, &cl);
	if (getenv("BTA2DP_DEBUG"))
		printf("SO_SNDTIMEO: rc=%d (%s), читается %ld с\n", rc,
		       rc ? strerror(errno) : "ок", (long)chk.tv_sec);
	if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		/* Ядро 3.0 возвращает EINPROGRESS, не дожидаясь установки
		 * ACL-канала (страничный вызов колонки — секунды). Ждём сами:
		 * готовность на запись и потом SO_ERROR. Если просто выйти,
		 * ядро отменит вызов, а в таблице повиснет полуоткрытый
		 * канал, который заблокирует следующие попытки. */
		if (errno != EINPROGRESS && errno != EAGAIN) {
			printf("соединение с PSM %d: %s\n", psm, strerror(errno));
			close(s);
			return -1;
		}
		struct pollfd pf = { s, POLLOUT, 0 };
		int pr = poll(&pf, 1, 40000);
		int err = 0;
		socklen_t el = sizeof(err);
		getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
		if (pr <= 0 || err) {
			printf("соединение с PSM %d: %s\n", psm,
			       pr <= 0 ? "нет ответа 40 с" : strerror(err));
			close(s);
			return -1;
		}
	}
	struct l2cap_options_ o;
	socklen_t ol = sizeof(o);
	memset(&o, 0, sizeof(o));
	if (getsockopt(s, SOL_L2CAP_, L2CAP_OPTIONS_, &o, &ol) == 0 && o.omtu)
		*mtu = o.omtu;
	else
		*mtu = 672;
	return s;
}

/* команда AVDTP; ответ кладём в rsp, возвращаем длину тела или -1 */
static int avdtp_cmd(int signal, const unsigned char *pl, int n,
		     unsigned char *rsp, int rmax)
{
	unsigned char pkt[300];
	label = (label + 1) & 0x0f;
	pkt[0] = (label << 4) | 0x00;        /* single, command */
	pkt[1] = signal;
	if (n)
		memcpy(pkt + 2, pl, n);
	if (write(sig_fd, pkt, 2 + n) < 0)
		return -1;
	for (int tries = 0; tries < 10; tries++) {
		unsigned char d[700];
		int r = read(sig_fd, d, sizeof(d));
		if (r < 2)
			return -1;
		int lbl = d[0] >> 4, type = d[0] & 3;
		if (lbl != label || d[1] != signal)
			continue;                    /* чужое — пропускаем */
		if (type == 2) {                     /* accept */
			int body = r - 2;
			if (body > rmax)
				body = rmax;
			memcpy(rsp, d + 2, body);
			return body;
		}
		if (type == 3) {                     /* reject */
			printf("сигнал %d отвергнут, код 0x%02x\n", signal,
			       r > 2 ? d[r - 1] : 0);
			return -1;
		}
	}
	return -1;
}

int main(int argc, char **argv)
{
	if (argc < 2 || !parse_mac(argv[1], dst)) {
		printf("использование: bta2dp <MAC> [файл.wav | -]\n");
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);
	int mtu;

	/* 1. сигнализация */
	sig_fd = l2_connect(AVDTP_PSM, &mtu);
	if (sig_fd < 0)
		return 1;
	printf("канал сигнализации открыт\n");
	struct timeval tv = {3, 0};
	setsockopt(sig_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* 2. DISCOVER */
	unsigned char rsp[300];
	int n = avdtp_cmd(DISCOVER, NULL, 0, rsp, sizeof(rsp));
	if (n < 2) {
		printf("колонка не ответила на DISCOVER\n");
		return 1;
	}
	int seid = -1;
	for (int i = 0; i + 1 < n; i += 2) {
		int id = rsp[i] >> 2, inuse = (rsp[i] >> 1) & 1;
		int media = rsp[i + 1] >> 4, tsep = (rsp[i + 1] >> 3) & 1;
		printf("SEP %d: %s%s%s\n", id, media == 0 ? "звук" : "не звук",
		       tsep ? ", приёмник" : ", источник",
		       inuse ? ", занят" : "");
		if (seid < 0 && media == 0 && tsep && !inuse)
			seid = id;
	}
	if (seid < 0) {
		printf("свободного звукового приёмника нет\n");
		return 1;
	}

	/* 3. GET_CAPABILITIES */
	unsigned char q[1] = {(unsigned char)(seid << 2)};
	n = avdtp_cmd(GET_CAPS, q, 1, rsp, sizeof(rsp));
	if (n < 0) {
		printf("нет ответа на GET_CAPABILITIES\n");
		return 1;
	}
	int have_sbc = 0, c0 = 0, c1 = 0, bpmin = 2, bpmax = 53;
	for (int i = 0; i + 1 < n;) {
		int cat = rsp[i], len = rsp[i + 1];
		if (cat == 7 && len >= 6 && rsp[i + 3] == 0) {   /* codec SBC */
			have_sbc = 1;
			c0 = rsp[i + 4];
			c1 = rsp[i + 5];
			bpmin = rsp[i + 6];
			bpmax = rsp[i + 7];
		}
		i += 2 + len;
	}
	if (!have_sbc) {
		printf("SEP %d не умеет SBC\n", seid);
		return 1;
	}
	printf("SBC у SEP %d: частоты%s%s%s%s, режимы%s%s%s%s, "
	       "блоки/подполосы 0x%02x, битпул %d..%d\n",
	       seid, c0 & 0x80 ? " 16k" : "", c0 & 0x40 ? " 32k" : "",
	       c0 & 0x20 ? " 44.1k" : "", c0 & 0x10 ? " 48k" : "",
	       c0 & 8 ? " моно" : "", c0 & 4 ? " dual" : "",
	       c0 & 2 ? " стерео" : "", c0 & 1 ? " joint" : "", c1,
	       bpmin, bpmax);
	if (argc < 3) {
		printf("разведка закончена\n");
		return 0;
	}

	/* 4. SET_CONFIGURATION: 44100, joint (или стерео), 16 блоков,
	 *    8 подполос, loudness; битпул — сколько колонка позволяет,
	 *    но не выше 53 (A2DP рекомендует 53 для стерео) */
	if (!(c0 & 0x20)) {
		printf("колонка не умеет 44100 — а у нас только он\n");
		return 1;
	}
	int mode = (c0 & 1) ? 0x01 : (c0 & 2) ? 0x02 : 0;
	if (!mode) {
		printf("колонка не умеет стерео/joint\n");
		return 1;
	}
	int bp = bpmax < 53 ? bpmax : 53;
	if (bp < bpmin)
		bp = bpmin;
	unsigned char conf[] = {
		(unsigned char)(seid << 2), (unsigned char)(1 << 2), /* acp, int */
		1, 0,                                  /* media transport */
		7, 6, 0x00, 0x00,                      /* media codec: звук, SBC */
		(unsigned char)(0x20 | mode),          /* 44100 + режим */
		0x15,                                  /* 16 блоков, 8 подполос, loudness */
		(unsigned char)bp, (unsigned char)bp
	};
	if (avdtp_cmd(SET_CONF, conf, sizeof(conf), rsp, sizeof(rsp)) < 0) {
		printf("колонка не приняла конфигурацию\n");
		return 1;
	}
	printf("конфигурация принята: 44100, %s, битпул %d\n",
	       mode == 1 ? "joint stereo" : "stereo", bp);

	/* 5. OPEN и канал данных */
	if (avdtp_cmd(OPEN, q, 1, rsp, sizeof(rsp)) < 0) {
		printf("OPEN отвергнут\n");
		return 1;
	}
	int mmtu;
	media_fd = l2_connect(AVDTP_PSM, &mmtu);
	if (media_fd < 0) {
		printf("канал данных не открылся\n");
		return 1;
	}
	printf("канал данных открыт, MTU %d\n", mmtu);

	/* 6. START */
	if (avdtp_cmd(START, q, 1, rsp, sizeof(rsp)) < 0) {
		printf("START отвергнут\n");
		return 1;
	}
	printf("поток запущен\n");
	fflush(stdout);

	/* кодек */
	sbc_t sbc;
	sbc_init(&sbc, 0);
	sbc.frequency = SBC_FREQ_44100;
	sbc.blocks = SBC_BLK_16;
	sbc.subbands = SBC_SB_8;
	sbc.mode = mode == 1 ? SBC_MODE_JOINT_STEREO : SBC_MODE_STEREO;
	sbc.allocation = SBC_AM_LOUDNESS;
	sbc.bitpool = bp;
	sbc.endian = SBC_LE;
	size_t codesize = sbc_get_codesize(&sbc);      /* PCM на кадр */
	size_t framelen = sbc_get_frame_length(&sbc);
	int frames_per_pkt = (mmtu - 13) / framelen;
	if (frames_per_pkt < 1)
		frames_per_pkt = 1;
	if (frames_per_pkt > 15)
		frames_per_pkt = 15;
	printf("кадр SBC: %zu байт PCM -> %zu байт, в пакете %d кадров\n",
	       codesize, framelen, frames_per_pkt);

	/* источник PCM */
	FILE *in;
	if (!strcmp(argv[2], "-"))
		in = stdin;
	else {
		in = fopen(argv[2], "rb");
		if (!in) {
			printf("нет файла %s\n", argv[2]);
			return 1;
		}
		unsigned char hdr[44];
		if (fread(hdr, 1, 44, in) != 44 || memcmp(hdr, "RIFF", 4)) {
			printf("это не WAV\n");
			return 1;
		}
		int ch = hdr[22] | (hdr[23] << 8);
		int rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16);
		int bits = hdr[34] | (hdr[35] << 8);
		if (ch != 2 || rate != 44100 || bits != 16) {
			printf("нужен WAV 44100/стерео/16 бит, а тут %d/%d/%d\n",
			       rate, ch, bits);
			return 1;
		}
	}

	unsigned char *pcm = malloc(codesize * frames_per_pkt);
	unsigned char *pkt = malloc(mmtu);
	unsigned short seq = 0;
	unsigned timestamp = 0;
	double t0 = now_s();
	unsigned long sent_frames = 0;
	int samples_per_frame = 16 * 8;               /* блоков x подполос */
	for (;;) {
		size_t got = fread(pcm, 1, codesize * frames_per_pkt, in);
		if (got < codesize)
			break;
		int nf = got / codesize;
		/* RTP-заголовок */
		pkt[0] = 0x80;
		pkt[1] = 0x60;                              /* payload type 96 */
		pkt[2] = seq >> 8; pkt[3] = seq & 0xff;
		pkt[4] = timestamp >> 24; pkt[5] = timestamp >> 16;
		pkt[6] = timestamp >> 8; pkt[7] = timestamp;
		pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;    /* ssrc */
		pkt[12] = nf;                               /* SBC: число кадров */
		int off = 13;
		for (int f = 0; f < nf; f++) {
			ssize_t w = 0;
			int r = sbc_encode(&sbc, pcm + f * codesize, codesize,
					   pkt + off, mmtu - off, &w);
			if (r < 0) {
				printf("ошибка кодера\n");
				goto done;
			}
			off += w;
		}
		/* ждём своего времени: кадр = 128 отсчётов при 44100 */
		double due = t0 + (double)sent_frames * samples_per_frame / 44100.0;
		double wait = due - now_s();
		if (wait > 0)
			usleep((useconds_t)(wait * 1e6));
		if (write(media_fd, pkt, off) < 0) {
			printf("колонка оборвала поток: %s\n", strerror(errno));
			break;
		}
		seq++;
		timestamp += nf * samples_per_frame;
		sent_frames += nf;
		if (sent_frames % 1000 < (unsigned)nf) {
			printf("передано %.1f с\n",
			       (double)sent_frames * samples_per_frame / 44100.0);
			fflush(stdout);
		}
	}
done:
	printf("конец потока, %.1f с\n",
	       (double)sent_frames * samples_per_frame / 44100.0);
	avdtp_cmd(SUSPEND, q, 1, rsp, sizeof(rsp));
	avdtp_cmd(CLOSE, q, 1, rsp, sizeof(rsp));
	sbc_finish(&sbc);
	close(media_fd);
	close(sig_fd);
	return 0;
}
