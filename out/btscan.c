/* btscan — поиск Bluetooth-устройств для HTC HD2 (BCM4329, ядро 3.0).
 *
 * Штатный hcitool тут не годится: чип шлёт результаты опроса, но
 * событие «опрос завершён» до хоста не доходит, и ioctl ядра висит до
 * таймаута. Поэтому говорим с контроллером напрямую по сырому
 * HCI-сокету: сами шлём Inquiry, сами собираем ответы и уже ПОСЛЕ
 * опроса запрашиваем имена — во время инквайри чип их отклоняет.
 *
 * btscan [секунд]  — печатает «MAC  RSSI  имя», по строке на устройство.
 *
 * Сборка: gcc -O2 btscan.c -o btscan
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

#define EVT_INQUIRY_RESULT          0x02
#define EVT_REMOTE_NAME_COMPLETE    0x07
#define EVT_INQUIRY_RESULT_RSSI     0x22
#define EVT_EXTENDED_INQUIRY_RESULT 0x2f

#define OGF_LINK_CTL        0x01
#define OCF_INQUIRY         0x0001
#define OCF_REMOTE_NAME_REQ 0x0019

#define MAXDEV 32

struct dev {
	unsigned char raw[6];
	char mac[18];
	int rssi;                 /* -128 — уровень не сообщили */
	char name[64];
};

static struct dev found[MAXDEV];
static int nfound;

static double now_s(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void mac_str(const unsigned char *raw, char *out)
{
	sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
		raw[5], raw[4], raw[3], raw[2], raw[1], raw[0]);
}

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
	struct timeval tv = {1, 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return s;
}

static void send_cmd(int s, int ogf, int ocf, const unsigned char *pl, int n)
{
	unsigned short op = (ogf << 10) | ocf;
	unsigned char pkt[64];
	pkt[0] = 0x01;
	pkt[1] = op & 0xff;
	pkt[2] = op >> 8;
	pkt[3] = n;
	if (n)
		memcpy(pkt + 4, pl, n);
	if (write(s, pkt, 4 + n) < 0) { /* нет связи с контроллером */ }
}

static int add_dev(const unsigned char *addr, int rssi)
{
	char m[18];
	mac_str(addr, m);
	for (int i = 0; i < nfound; i++)
		if (!strcmp(found[i].mac, m))
			return i;
	if (nfound >= MAXDEV)
		return -1;
	memcpy(found[nfound].raw, addr, 6);
	strcpy(found[nfound].mac, m);
	found[nfound].rssi = rssi;
	found[nfound].name[0] = 0;
	return nfound++;
}

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 12;
	if (seconds < 2)
		seconds = 2;
	int s = hci_open();
	if (s < 0) {
		fprintf(stderr, "нет доступа к HCI: включён ли Bluetooth?\n");
		return 1;
	}

	/* Inquiry: LAP 9e8b33 (общий), длительность, без лимита ответов */
	int dur = seconds / 1.28;
	if (dur < 2) dur = 2;
	if (dur > 30) dur = 30;
	unsigned char inq[5] = {0x33, 0x8b, 0x9e, (unsigned char)dur, 0x00};
	send_cmd(s, OGF_LINK_CTL, OCF_INQUIRY, inq, 5);

	double t0 = now_s();
	while (now_s() - t0 < seconds) {
		unsigned char d[300];
		int r = read(s, d, sizeof(d));
		if (r < 3 || d[0] != 0x04)          /* только события */
			continue;
		int code = d[1];
		unsigned char *body = d + 3;
		int blen = r - 3;
		if (code != EVT_INQUIRY_RESULT && code != EVT_INQUIRY_RESULT_RSSI &&
		    code != EVT_EXTENDED_INQUIRY_RESULT)
			continue;
		int n = (code == EVT_EXTENDED_INQUIRY_RESULT) ? 1 : body[0];
		int off = 1;
		for (int i = 0; i < n; i++) {
			if (off + 14 > blen)
				break;
			int rssi = -128;
			if (code == EVT_INQUIRY_RESULT_RSSI ||
			    code == EVT_EXTENDED_INQUIRY_RESULT)
				rssi = (signed char)body[off + 13];
			add_dev(body + off, rssi);
			off += 14;
		}
	}

	/* имена спрашиваем ПОСЛЕ опроса */
	for (int i = 0; i < nfound; i++) {
		unsigned char pl[10];
		memcpy(pl, found[i].raw, 6);
		pl[6] = 0x02; pl[7] = 0x00; pl[8] = 0x00; pl[9] = 0x00;
		send_cmd(s, OGF_LINK_CTL, OCF_REMOTE_NAME_REQ, pl, 10);
		double t1 = now_s();
		while (now_s() - t1 < 6) {
			unsigned char d[300];
			int r = read(s, d, sizeof(d));
			if (r < 11 || d[0] != 0x04 ||
			    d[1] != EVT_REMOTE_NAME_COMPLETE)
				continue;
			char m[18];
			mac_str(d + 4, m);            /* d+3 — статус, дальше адрес */
			if (strcmp(m, found[i].mac))
				continue;
			int len = r - 10;
			if (len > (int)sizeof(found[i].name) - 1)
				len = sizeof(found[i].name) - 1;
			memcpy(found[i].name, d + 10, len);
			found[i].name[len] = 0;
			break;
		}
	}
	close(s);

	if (!nfound) {
		printf("устройств не найдено\n");
		return 1;
	}
	/* сильные сверху */
	for (int i = 0; i < nfound; i++)
		for (int j = i + 1; j < nfound; j++)
			if (found[j].rssi > found[i].rssi) {
				struct dev t = found[i];
				found[i] = found[j];
				found[j] = t;
			}
	for (int i = 0; i < nfound; i++) {
		char lvl[16];
		if (found[i].rssi == -128)
			strcpy(lvl, "—");
		else
			snprintf(lvl, sizeof(lvl), "%d дБм", found[i].rssi);
		printf("%s  %8s  %s\n", found[i].mac, lvl,
		       found[i].name[0] ? found[i].name : "(без имени)");
	}
	return 0;
}
