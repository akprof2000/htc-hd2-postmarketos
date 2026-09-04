/* btdebug — что на самом деле присылает контроллер во время поиска.
 *
 * Нужен для разбирательства: свой сканер находит устройства не всегда, а
 * готовых hcidump/btmon в системе нет. Программа отменяет возможный
 * застрявший опрос, запускает новый и печатает КАЖДОЕ событие HCI с
 * кодом и длиной, чтобы видеть, доходят ли ответы вообще.
 *
 * btdebug [секунд]
 *
 * Сборка: gcc -O2 btdebug.c -o btdebug
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define AF_BLUETOOTH_ 31
#define BTPROTO_HCI_  1
#define HCI_FILTER_   2

struct sockaddr_hci_ {
	unsigned short family;
	unsigned short dev;
	unsigned short channel;
};

static int hci = -1;

static double now_s(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void cmd(int ogf, int ocf, const unsigned char *pl, int n,
		const char *name)
{
	unsigned short op = (ogf << 10) | ocf;
	unsigned char pkt[64];
	pkt[0] = 0x01;
	pkt[1] = op & 0xff;
	pkt[2] = op >> 8;
	pkt[3] = n;
	if (n)
		memcpy(pkt + 4, pl, n);
	if (write(hci, pkt, 4 + n) < 0)
		printf("не отправилась команда %s\n", name);
	else
		printf("-> %s (opcode 0x%04x)\n", name, op);
	fflush(stdout);
}

static const char *evt_name(int code)
{
	switch (code) {
	case 0x01: return "Inquiry Complete";
	case 0x02: return "Inquiry Result";
	case 0x03: return "Connection Complete";
	case 0x04: return "Connection Request";
	case 0x05: return "Disconnection Complete";
	case 0x07: return "Remote Name Complete";
	case 0x0e: return "Command Complete";
	case 0x0f: return "Command Status";
	case 0x13: return "Number Of Completed Packets";
	case 0x22: return "Inquiry Result with RSSI";
	case 0x2f: return "Extended Inquiry Result";
	default:   return "прочее";
	}
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 20;
	hci = socket(AF_BLUETOOTH_, SOCK_RAW, BTPROTO_HCI_);
	if (hci < 0) {
		printf("нет доступа к HCI\n");
		return 1;
	}
	unsigned f[4] = {0xffffffff, 0xffffffff, 0xffffffff, 0};
	setsockopt(hci, 0, HCI_FILTER_, f, sizeof(f));
	struct sockaddr_hci_ a;
	memset(&a, 0, sizeof(a));
	a.family = AF_BLUETOOTH_;
	a.dev = 0;
	a.channel = 0;
	if (bind(hci, (struct sockaddr *)&a, sizeof(a)) < 0) {
		printf("не удалось привязаться к hci0\n");
		return 1;
	}
	struct timeval tv = {1, 0};
	setsockopt(hci, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* сначала отменяем возможный застрявший опрос: этот чип не шлёт
	 * «опрос завершён», и контроллер может считать себя занятым */
	cmd(0x01, 0x0002, NULL, 0, "Inquiry Cancel");
	usleep(300000);
	/* явно разрешаем ВСЕ события: если маска событий пришла из
	 * прошивки урезанной, результаты опроса до хоста не дойдут */
	unsigned char mask[8];
	memset(mask, 0xff, sizeof(mask));
	cmd(0x03, 0x0001, mask, 8, "Set Event Mask (все)");
	usleep(300000);
	/* режим опроса: 0 — обычные результаты, 1 — с уровнем сигнала */
	unsigned char im[1] = {0x01};
	cmd(0x03, 0x0045, im, 1, "Write Inquiry Mode (с RSSI)");
	usleep(300000);
	unsigned char inq[5] = {0x33, 0x8b, 0x9e, 0x08, 0x00};
	cmd(0x01, 0x0001, inq, 5, "Inquiry");

	double t0 = now_s();
	int total = 0;
	while (now_s() - t0 < secs) {
		unsigned char d[300];
		int r = read(hci, d, sizeof(d));
		if (r <= 0)
			continue;
		total++;
		if (d[0] != 0x04) {
			printf("<- пакет типа 0x%02x, %d байт\n", d[0], r);
			continue;
		}
		printf("<- событие 0x%02x %-28s %d байт", d[1], evt_name(d[1]), r);
		if (d[1] == 0x02 || d[1] == 0x22 || d[1] == 0x2f) {
			printf("  адрес %02X:%02X:%02X:%02X:%02X:%02X",
			       d[9], d[8], d[7], d[6], d[5], d[4]);
		}
		if (d[1] == 0x0e && r >= 7)
			printf("  на 0x%04x статус %d", d[4] | (d[5] << 8), d[6]);
		if (d[1] == 0x0f && r >= 7)
			printf("  статус %d на 0x%04x", d[3], d[5] | (d[6] << 8));
		printf("\n");
		fflush(stdout);
	}
	printf("всего событий: %d\n", total);
	close(hci);
	return 0;
}
