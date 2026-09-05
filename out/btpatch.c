/* btpatch — заливка прошивки в Bluetooth-чип BCM4329 напрямую по HCI.
 *
 * ЗАЧЕМ: штатный hciattach заплатку не находит («Patch not found for
 * BCM4329B1, continue anyway»), сколько её ни раскладывай по каталогам,
 * и чип остаётся на базовом ПЗУ. Команды такой чип выполняет, а радио
 * молчит: телефон никого не видит, и его самого не видно, хотя в
 * Windows Mobile тот же Bluetooth работает. Здесь заплатка грузится
 * сама, без поиска по файловой системе.
 *
 * Порядок ровно тот, что делает hciattach: Download Minidriver
 * (0xFC2E), затем все записи файла как обычные HCI-команды, затем
 * HCI Reset. Файл .hcd — последовательность записей вида
 * [опкод младший, опкод старший, длина, данные…].
 *
 *     btpatch [файл.hcd]     по умолчанию /lib/firmware/BCM4329B1.hcd
 *
 * Сборка: gcc -O2 btpatch.c -o btpatch
 */
#include <fcntl.h>
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

/* отправить команду и дождаться Command Complete; вернуть статус
 * (0 — успех), -1 — ответа нет */
static int send_cmd(unsigned short op, const unsigned char *pl, int n)
{
	unsigned char pkt[300];
	pkt[0] = 0x01;
	pkt[1] = op & 0xff;
	pkt[2] = op >> 8;
	pkt[3] = n;
	if (n)
		memcpy(pkt + 4, pl, n);
	if (write(hci, pkt, 4 + n) < 0)
		return -1;
	for (int tries = 0; tries < 20; tries++) {
		unsigned char d[300];
		int r = read(hci, d, sizeof(d));
		if (r < 7)
			continue;
		if (d[0] != 0x04 || d[1] != 0x0e)      /* Command Complete */
			continue;
		unsigned short rop = d[4] | (d[5] << 8);
		if (rop != op)
			continue;
		return d[6];
	}
	return -1;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/lib/firmware/BCM4329B1.hcd";
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("нет файла прошивки: %s\n", path);
		return 1;
	}

	hci = socket(AF_BLUETOOTH_, SOCK_RAW, BTPROTO_HCI_);
	if (hci < 0) {
		printf("нет доступа к HCI\n");
		return 1;
	}
	unsigned flt[4] = {0xffffffff, 0xffffffff, 0xffffffff, 0};
	setsockopt(hci, 0, HCI_FILTER_, flt, sizeof(flt));
	struct sockaddr_hci_ a;
	memset(&a, 0, sizeof(a));
	a.family = AF_BLUETOOTH_;
	a.dev = 0;
	a.channel = 0;
	if (bind(hci, (struct sockaddr *)&a, sizeof(a)) < 0) {
		printf("не удалось привязаться к hci0\n");
		return 1;
	}
	struct timeval tv = {2, 0};
	setsockopt(hci, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* 1. перевод чипа в режим загрузки */
	int st = send_cmd(0xFC2E, NULL, 0);
	if (st != 0) {
		printf("чип не перешёл в режим загрузки (ответ %d)\n", st);
		return 1;
	}
	printf("режим загрузки включён\n");
	usleep(50000);

	/* 2. записи файла — обычные HCI-команды */
	int total = 0, bad = 0, miss = 0;
	for (;;) {
		unsigned char h[3];
		if (fread(h, 1, 3, f) != 3)
			break;
		int len = h[2];
		unsigned char data[256];
		if (len && fread(data, 1, len, f) != (size_t)len) {
			printf("файл оборвался на записи %d\n", total);
			break;
		}
		unsigned short op = h[0] | (h[1] << 8);
		/* Записи повторяем: ответ на этом чипе иногда теряется, а
		 * пропущенная запись оставляет прошивку наполовину залитой —
		 * тогда радио ведёт себя странно: вызвать может, а на чужой
		 * поиск не отзывается. Между записями короткая пауза: без неё
		 * чип захлёбывается примерно на середине файла. */
		int r = -1;
		for (int t = 0; t < 3 && r != 0; t++) {
			if (t)
				usleep(30000);
			r = send_cmd(op, data, len);
		}
		usleep(2000);
		total++;
		if (r != 0) {
			bad++;
			miss++;
			if (bad <= 3)
				printf("запись %d (опкод 0x%04x): ответ %d\n",
				       total, op, r);
			/* Пять отказов подряд — чип уже не слушает; лить
			 * дальше бессмысленно и вредно. */
			if (miss >= 5) {
				printf("чип перестал отвечать на записи %d — "
				       "останавливаюсь\n", total);
				break;
			}
		} else {
			miss = 0;
		}
		if (total % 50 == 0) {
			printf("залито записей: %d\n", total);
			fflush(stdout);
		}
	}
	fclose(f);
	printf("всего записей: %d, с ошибкой: %d\n", total, bad);

	/* 3. перезапуск контроллера, чтобы заплатка вступила в силу */
	usleep(50000);
	st = send_cmd(0x0C03, NULL, 0);          /* HCI Reset */
	printf("перезапуск контроллера: ответ %d\n", st);
	usleep(200000);

	/* 4. проверяем, что заплатка легла: у чистого ПЗУ ревизия 0x0000,
	 * с заплаткой — ненулевая (у нас 0x02f0) */
	unsigned char ver[4] = {0x01, 0x01, 0x10, 0x00};
	if (write(hci, ver, 4) == 4) {
		for (int t = 0; t < 20; t++) {
			unsigned char d[300];
			int r = read(hci, d, sizeof(d));
			if (r < 14 || d[0] != 0x04 || d[1] != 0x0e)
				continue;
			if ((d[4] | (d[5] << 8)) != 0x1001)
				continue;
			unsigned rev = d[8] | (d[9] << 8);
			printf("ревизия прошивки: 0x%04x %s\n", rev,
			       rev ? "— заплатка на месте" : "— ЗАПЛАТКИ НЕТ");
			break;
		}
	}
	close(hci);
	return bad ? 1 : 0;
}
