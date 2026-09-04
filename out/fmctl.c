/* fmctl — управление FM-приёмником HTC HD2 из командной строки.
 *
 * Приёмник сидит в чипе BCM4329 (там же Bluetooth и Wi-Fi) и доступен
 * vendor-командой HCI 0xFC15: [регистр, 0, данные…] — запись,
 * [регистр, 1, размер] — чтение. Карта регистров как у BCM2048.
 *
 * КЛЮЧЕВОЕ: регистр частоты 0x0a — это КИЛОГЕРЦЫ МИНУС 64000, шаг 1 кГц,
 * а НЕ шаги по 10 кГц из карты BCM2048. Из-за деления на 10 приёмник
 * когда-то месяц уезжал мимо станций.
 *
 * ОПАСНО: опкоды 0xFC16/0xFC17/0xFC14 вешают чип и драйвер UART (лечит
 * только перезагрузка по питанию); два процесса одновременно на 0xFC15
 * дают статус 0x0c — поэтому не запускать одновременно с радио.
 *
 * Команды:
 *   fmctl status            состояние регистров, частота, уровень
 *   fmctl on [кГц]          включить и настроить (по умолчанию 100500)
 *   fmctl off               выключить
 *   fmctl tune 101.2        настроить на частоту в МГц
 *   fmctl rssi              уровень сигнала
 *   fmctl scan              обзор диапазона по уровню, шаг 100 кГц
 *   fmctl seek [up|down]    аппаратный поиск станции
 *
 * Сборка: gcc -O2 fmctl.c -o fmctl
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define AF_BLUETOOTH_ 31
#define BTPROTO_HCI_  1
#define HCI_FILTER_   2

struct sockaddr_hci_ {
	unsigned short family;
	unsigned short dev;
	unsigned short channel;
};

#define FREQ_BASE 64000
#define BAND_LO   87500
#define BAND_HI   108000

/* регистры BCM2048 */
#define R_RDS_SYSTEM        0x00
#define R_FM_CTRL           0x01
#define R_AUDIO_CTRL0       0x05
#define R_SEARCH_TUNE_MODE  0x09
#define R_FREQ0             0x0a
#define R_SEARCH_CTRL0      0x07
#define R_FLAG0             0x12
#define R_RSSI              0x0f

static int hci = -1;

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

/* vendor-команда 0xFC15; ответ — параметры Command Complete */
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
		if (b[0] == 0x04 && b[1] == 0x0e) {       /* Command Complete */
			unsigned short rop = b[4] | (b[5] << 8);
			if (rop != op)
				continue;
			if (b[6] != 0)
				return -1;                /* чип отверг команду */
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

static void save_freq(int khz)
{
	int fd = open("/run/fm", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return;
	char b[16];
	int n = snprintf(b, sizeof(b), "%d", khz);
	if (write(fd, b, n) < 0) { }
	close(fd);
}

static void fm_on(void)
{
	fm_wr(0x00, 0x01, 1);
	usleep(50000);
	fm_wr(0x00, 0x01, 1);
	fm_wr(0x10, 0x0000, 2);
	fm_wr(0x01, 0x06, 1);              /* Европа, стерео + авто */
	fm_wr(0x14, 0x40, 1);
	fm_wr(0x05, 0x001c, 2);            /* 50 мкс, DAC L+R, без mute */
	fm_wr(0x4d, 0x40, 1);              /* аналоговый выход */
	fm_wr(0x04, 0, 1);
}

static void fm_off(void)
{
	fm_wr(R_RDS_SYSTEM, 0, 1);
}

static void fm_tune(int khz)
{
	fm_wr(R_FREQ0, khz - FREQ_BASE, 2);   /* килогерцы, не шаги по 10 */
	fm_wr(R_SEARCH_TUNE_MODE, 0x01, 1);   /* preset tune */
	usleep(200000);
}

static int fm_freq(void)
{
	int v = fm_rd(R_FREQ0, 2);
	return v < 0 ? -1 : v + FREQ_BASE;
}

static int fm_rssi(void)
{
	int v = fm_rd(R_RSSI, 1);
	if (v < 0)
		return -128;
	return v - 144;                       /* шкала Spirit2: 0 — нет сигнала */
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "status";
	hci = hci_open();
	if (hci < 0) {
		fprintf(stderr, "нет доступа к HCI: включён ли Bluetooth?\n");
		return 1;
	}

	if (!strcmp(cmd, "on")) {
		fm_on();
		int khz = argc > 2 ? atoi(argv[2]) : 100500;
		fm_tune(khz);
		save_freq(khz);
		printf("FM включён, %.1f МГц, RSSI %d\n", fm_freq() / 1000.0,
		       fm_rssi());
	} else if (!strcmp(cmd, "off")) {
		fm_off();
		save_freq(0);
		printf("FM выключен\n");
	} else if (!strcmp(cmd, "tune") && argc > 2) {
		int khz = (int)(atof(argv[2]) * 1000 + 0.5);
		fm_tune(khz);
		save_freq(khz);
		printf("%.1f МГц, RSSI %d\n", fm_freq() / 1000.0, fm_rssi());
	} else if (!strcmp(cmd, "rssi")) {
		printf("%d\n", fm_rssi());
	} else if (!strcmp(cmd, "status")) {
		printf("RDS_SYSTEM=0x%02x FM_CTRL=0x%02x AUDIO=0x%02x "
		       "FLAG=0x%02x %.1f МГц RSSI %d\n",
		       fm_rd(R_RDS_SYSTEM, 1), fm_rd(R_FM_CTRL, 1),
		       fm_rd(R_AUDIO_CTRL0, 1), fm_rd(R_FLAG0, 1),
		       fm_freq() / 1000.0, fm_rssi());
	} else if (!strcmp(cmd, "scan")) {
		int best_k[8] = {0}, best_r[8];
		for (int i = 0; i < 8; i++)
			best_r[i] = -128;
		for (int khz = BAND_LO; khz <= BAND_HI; khz += 100) {
			fm_tune(khz);
			int r = fm_rssi();
			if (r > -60)
				printf("%.1f МГц  %d дБм\n", khz / 1000.0, r);
			for (int i = 0; i < 8; i++)   /* восьмёрка лучших */
				if (r > best_r[i]) {
					for (int j = 7; j > i; j--) {
						best_r[j] = best_r[j - 1];
						best_k[j] = best_k[j - 1];
					}
					best_r[i] = r;
					best_k[i] = khz;
					break;
				}
		}
		printf("самые сильные:");
		for (int i = 0; i < 8 && best_k[i]; i++)
			printf(" %.1f(%d)", best_k[i] / 1000.0, best_r[i]);
		printf("\n");
	} else if (!strcmp(cmd, "seek")) {
		int up = !(argc > 2 && !strcmp(argv[2], "down"));
		/* как в Spirit2: 0x07 — порог RSSI и бит 7 направление,
		 * 0x09 = 2 — авто-поиск; ждём флаг завершения */
		fm_wr(0xfc, 0x00, 1);
		fm_wr(R_SEARCH_CTRL0, 0x60 | (up ? 0x80 : 0x00), 1);
		fm_wr(R_SEARCH_TUNE_MODE, 0x02, 1);
		for (int i = 0; i < 60; i++) {
			usleep(100000);
			if (fm_rd(R_FLAG0, 2) & 0x01)
				break;
		}
		int khz = fm_freq();
		save_freq(khz);
		printf("%.1f МГц, RSSI %d, флаги 0x%04x\n", khz / 1000.0,
		       fm_rssi(), fm_rd(R_FLAG0, 2));
	} else {
		fprintf(stderr, "команды: status | on [кГц] | off | tune МГц | "
			"rssi | scan | seek [up|down]\n");
		return 2;
	}
	close(hci);
	return 0;
}
