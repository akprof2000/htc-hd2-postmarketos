/* btconn — проверка одного шага: установить ACL-соединение с
 * устройством и сразу разорвать. Ни сопряжения, ни ключей, ни PIN.
 *
 * Нужен, чтобы отделить «телефон не умеет вызывать» от «телефон не
 * отвечает на вызов»: btpair делает и то и другое сразу, и по его
 * неудаче не понять, какая половина сломана.
 *
 *     btconn <MAC>
 *
 * Сборка: gcc -O2 btconn.c -o btconn
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

static void send_cmd(unsigned short op, const unsigned char *pl, int n)
{
	unsigned char pkt[64];
	pkt[0] = 0x01;
	pkt[1] = op & 0xff;
	pkt[2] = op >> 8;
	pkt[3] = n;
	if (n)
		memcpy(pkt + 4, pl, n);
	if (write(hci, pkt, 4 + n) < 0)
		printf("не удалось отправить команду\n");
}

static double now_s(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("использование: btconn <MAC>\n");
		return 2;
	}
	unsigned v[6];
	if (sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3],
		   &v[4], &v[5]) != 6) {
		printf("непонятный адрес\n");
		return 2;
	}
	unsigned char dst[6];
	for (int i = 0; i < 6; i++)
		dst[i] = (unsigned char)v[5 - i];

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
	if (bind(hci, (struct sockaddr *)&a, sizeof(a)) < 0) {
		printf("не удалось привязаться к hci0\n");
		return 1;
	}
	struct timeval tv = {1, 0};
	setsockopt(hci, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	unsigned char conn[13];
	memcpy(conn, dst, 6);
	conn[6] = 0x18; conn[7] = 0xcc;   /* пакеты DM1..DH5 */
	/* режим повторения страничного сканирования: R2 по умолчанию,
	 * вторым аргументом можно задать 0 или 1 — на некоторых устройствах
	 * вызов проходит только с «их» режимом */
	conn[8] = argc > 2 ? (unsigned char)atoi(argv[2]) : 0x02;
	conn[9] = 0x00;
	conn[10] = 0x00; conn[11] = 0x00; /* clock offset неизвестен */
	conn[12] = 0x01;                  /* смена роли разрешена */
	send_cmd(0x0405, conn, 13);
	printf("вызываю %s…\n", argv[1]);
	fflush(stdout);

	double t0 = now_s();
	while (now_s() - t0 < 30) {
		unsigned char d[300];
		int r = read(hci, d, sizeof(d));
		if (r < 3 || d[0] != 0x04)
			continue;
		unsigned char *b = d + 3;
		if (d[1] == 0x0f && r >= 7) {           /* Command Status */
			unsigned short op = b[2] | (b[3] << 8);
			if (op == 0x0405)
				printf("контроллер принял команду, код %d\n", b[0]);
		} else if (d[1] == 0x03 && r >= 12) {   /* Connection Complete */
			if (b[0] == 0) {
				unsigned short h = b[1] | (b[2] << 8);
				printf("СОЕДИНЕНИЕ УСТАНОВЛЕНО (ручка %d)\n", h);
				unsigned char p[3] = {b[1], b[2], 0x13};
				send_cmd(0x0406, p, 3);
				sleep(1);
				close(hci);
				return 0;
			}
			printf("отказ, код %d ", b[0]);
			if (b[0] == 0x04)
				printf("(страничный вызов без ответа)");
			else if (b[0] == 0x0f)
				printf("(соединение запрещено)");
			printf("\n");
			close(hci);
			return 1;
		}
	}
	printf("время вышло — ответа нет\n");
	close(hci);
	return 1;
}
