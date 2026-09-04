/* btlisten — пассивный слушатель HCI: ничего не посылает, только
 * печатает события контроллера. Нужен, чтобы увидеть, доходит ли до
 * телефона входящее подключение: btdebug для этого не годится — он сам
 * запускает поиск, а во время поиска телефон на подключение не
 * отзывается.
 *
 * Сборка: gcc -O2 btlisten.c -o btlisten
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define AF_BLUETOOTH_ 31
#define BTPROTO_HCI_  1
#define HCI_FILTER_   2

struct sockaddr_hci_ {
	unsigned short family;
	unsigned short dev;
	unsigned short channel;
};

static const char *name_of(int code)
{
	switch (code) {
	case 0x03: return "Connection Complete";
	case 0x04: return "Connection Request";
	case 0x05: return "Disconnection Complete";
	case 0x06: return "Authentication Complete";
	case 0x0e: return "Command Complete";
	case 0x0f: return "Command Status";
	case 0x16: return "PIN Code Request";
	case 0x17: return "Link Key Request";
	case 0x18: return "Link Key Notification";
	case 0x31: return "IO Capability Request";
	case 0x33: return "User Confirmation Request";
	case 0x36: return "Simple Pairing Complete";
	default: return "";
	}
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 60;
	int s = socket(AF_BLUETOOTH_, SOCK_RAW, BTPROTO_HCI_);
	if (s < 0) {
		printf("нет доступа к HCI\n");
		return 1;
	}
	unsigned f[4] = {0xffffffff, 0xffffffff, 0xffffffff, 0};
	setsockopt(s, 0, HCI_FILTER_, f, sizeof(f));
	struct sockaddr_hci_ a;
	memset(&a, 0, sizeof(a));
	a.family = AF_BLUETOOTH_;
	a.dev = 0;
	a.channel = 0;
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		printf("не удалось привязаться к hci0\n");
		return 1;
	}
	struct timeval tv = {1, 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	printf("слушаю %d секунд, ничего не посылаю\n", secs);
	fflush(stdout);
	time_t end = time(NULL) + secs;
	int n = 0;
	while (time(NULL) < end) {
		unsigned char d[300];
		int r = read(s, d, sizeof(d));
		if (r < 3)
			continue;
		if (d[0] == 0x04) {
			const char *nm = name_of(d[1]);
			printf("<- событие 0x%02x %-26s %d байт\n", d[1], nm, r - 3);
			n++;
		} else if (d[0] == 0x01) {
			printf("-> команда 0x%04x\n", d[1] | (d[2] << 8));
		}
		fflush(stdout);
	}
	printf("всего событий: %d\n", n);
	return 0;
}
