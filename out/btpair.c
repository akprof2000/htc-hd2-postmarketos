/* btpair — сопряжение Bluetooth для HTC HD2 напрямую по HCI.
 *
 * BlueZ-агента на ядре 3.0 нет (mgmt-интерфейса не существует), поэтому
 * PIN-код и подтверждение обрабатываем сами: слушаем события
 * контроллера и отвечаем нужными командами.
 *
 *     btpair <MAC> [PIN]     — сопрячься (по умолчанию PIN 0000)
 *
 * Ключ связи сохраняет сам контроллер; после успеха с устройством можно
 * работать (передача файлов и прочее).
 *
 * Сборка: gcc -O2 btpair.c -o btpair
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

#define EVT_CONN_COMPLETE        0x03
#define EVT_AUTH_COMPLETE        0x06
#define EVT_PIN_CODE_REQ         0x16
#define EVT_LINK_KEY_REQ         0x17
#define EVT_LINK_KEY_NOTIFY      0x18
#define EVT_IO_CAP_REQ           0x31
#define EVT_USER_CONFIRM_REQ     0x33
#define EVT_SIMPLE_PAIR_COMPLETE 0x36
#define EVT_CMD_STATUS           0x0f

#define OGF_LINK_CTL           0x01
#define OCF_CREATE_CONN        0x0005
#define OCF_DISCONNECT         0x0006
#define OCF_LINK_KEY_NEG       0x000c
#define OCF_PIN_REPLY          0x000d
#define OCF_AUTH_REQ           0x0011
#define OCF_IO_CAP_REPLY       0x002b
#define OCF_USER_CONFIRM_REPLY 0x002c

static int hci = -1;

static double now_s(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void send_cmd(int ogf, int ocf, const unsigned char *pl, int n)
{
	unsigned short op = (ogf << 10) | ocf;
	unsigned char pkt[64];
	pkt[0] = 0x01;
	pkt[1] = op & 0xff;
	pkt[2] = op >> 8;
	pkt[3] = n;
	if (n)
		memcpy(pkt + 4, pl, n);
	if (write(hci, pkt, 4 + n) < 0) { /* нет связи с контроллером */ }
}

/* «AA:BB:…» -> шесть байт в порядке, который ждёт контроллер */
static int parse_mac(const char *s, unsigned char *out)
{
	unsigned v[6];
	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4],
		   &v[5]) != 6)
		return 0;
	for (int i = 0; i < 6; i++)
		out[i] = (unsigned char)v[5 - i];
	return 1;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("использование: btpair <MAC> [PIN]\n");
		return 2;
	}
	const char *mac = argv[1];
	const char *pin = argc > 2 ? argv[2] : "0000";
	unsigned char dst[6];
	if (!parse_mac(mac, dst)) {
		printf("непонятный адрес: %s\n", mac);
		return 2;
	}

	hci = socket(AF_BLUETOOTH_, SOCK_RAW, BTPROTO_HCI_);
	if (hci < 0) {
		fprintf(stderr, "нет доступа к HCI: включён ли Bluetooth?\n");
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
		fprintf(stderr, "не удалось привязаться к hci0\n");
		return 1;
	}
	struct timeval tv = {1, 0};
	setsockopt(hci, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* соединение: пакеты DM1/DH5, page scan R2, разрешить смену роли */
	unsigned char conn[13];
	memcpy(conn, dst, 6);
	conn[6] = 0x18; conn[7] = 0xcc;      /* тип пакетов 0xcc18 */
	conn[8] = 0x02;                      /* page scan repetition mode */
	conn[9] = 0x00;                      /* reserved */
	conn[10] = 0x00; conn[11] = 0x00;    /* clock offset */
	conn[12] = 0x01;                     /* allow role switch */
	send_cmd(OGF_LINK_CTL, OCF_CREATE_CONN, conn, 13);

	unsigned short handle = 0;
	double t0 = now_s();
	/* 90 секунд, а не 40: на этом чипе установка связи (paging) идёт
	 * заметно дольше обычного — соединение появлялось уже после того,
	 * как прежний срок истекал */
	while (now_s() - t0 < 90) {
		unsigned char d[300];
		int r = read(hci, d, sizeof(d));
		if (r < 3 || d[0] != 0x04)
			continue;
		int code = d[1];
		unsigned char *body = d + 3;
		int blen = r - 3;

		if (code == EVT_CMD_STATUS && blen >= 4) {
			/* контроллер сразу отверг команду — ждать нечего */
			unsigned short op = body[2] | (body[3] << 8);
			if (op == ((OGF_LINK_CTL << 10) | OCF_CREATE_CONN) &&
			    body[0]) {
				printf("контроллер отверг соединение (код %d)\n",
				       body[0]);
				close(hci);
				return 1;
			}
		} else if (code == EVT_CONN_COMPLETE && blen >= 9) {
			if (body[0]) {
				printf("соединиться не удалось (код %d)\n", body[0]);
				close(hci);
				return 1;
			}
			handle = body[1] | (body[2] << 8);
			printf("соединение установлено, начинаю сопряжение…\n");
			unsigned char h[2] = {body[1], body[2]};
			send_cmd(OGF_LINK_CTL, OCF_AUTH_REQ, h, 2);
		} else if (code == EVT_PIN_CODE_REQ) {
			printf("устройство просит PIN — отправляю %s\n", pin);
			unsigned char p[23];
			memcpy(p, dst, 6);
			int pl = strlen(pin);
			if (pl > 16)
				pl = 16;
			p[6] = (unsigned char)pl;
			memset(p + 7, 0, 16);
			memcpy(p + 7, pin, pl);
			send_cmd(OGF_LINK_CTL, OCF_PIN_REPLY, p, 23);
		} else if (code == EVT_LINK_KEY_REQ) {
			/* ключа ещё нет — «нет», это запустит запрос PIN */
			send_cmd(OGF_LINK_CTL, OCF_LINK_KEY_NEG, dst, 6);
		} else if (code == EVT_IO_CAP_REQ) {
			/* ни экрана, ни клавиатуры: NoInputNoOutput, без MITM */
			unsigned char p[9];
			memcpy(p, dst, 6);
			p[6] = 0x03; p[7] = 0x00; p[8] = 0x00;
			send_cmd(OGF_LINK_CTL, OCF_IO_CAP_REPLY, p, 9);
		} else if (code == EVT_USER_CONFIRM_REQ) {
			printf("подтверждаю сопряжение\n");
			send_cmd(OGF_LINK_CTL, OCF_USER_CONFIRM_REPLY, dst, 6);
		} else if (code == EVT_LINK_KEY_NOTIFY) {
			printf("ключ связи получен — устройство сопряжено\n");
		} else if (code == EVT_AUTH_COMPLETE ||
			   code == EVT_SIMPLE_PAIR_COMPLETE) {
			if (body[0] == 0) {
				printf("СОПРЯЖЕНО: %s\n", mac);
				if (handle) {
					unsigned char p[3] = {
						(unsigned char)(handle & 0xff),
						(unsigned char)(handle >> 8), 0x13};
					send_cmd(OGF_LINK_CTL, OCF_DISCONNECT, p, 3);
				}
				close(hci);
				return 0;
			}
			printf("сопряжение отклонено (код %d)\n", body[0]);
			close(hci);
			return 1;
		}
	}
	printf("время вышло — устройство не ответило\n");
	close(hci);
	return 1;
}
