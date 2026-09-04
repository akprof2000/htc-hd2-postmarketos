/* btagent — приём входящего сопряжения Bluetooth.
 *
 * На ядре 3.0 нет mgmt-интерфейса, а значит и обычного агента BlueZ:
 * телефон умеет сам подключаться (btpair), но когда сопрячься хотят С
 * НИМ, отвечать на запросы контроллера некому — сопряжение молча
 * проваливается. Этот сторож слушает события HCI и отвечает за нас:
 *
 *   Connection Request      -> принять
 *   Link Key Request        -> «ключа нет» (запустит запрос PIN)
 *   PIN Code Request        -> выдать PIN (по умолчанию 0000)
 *   IO Capability Request   -> «нет ни экрана, ни клавиатуры», без MITM
 *   User Confirmation       -> подтвердить
 *
 * Запуск: btagent [PIN]     — работает постоянно, пишет в /tmp/btagent.log
 *
 * Сборка: gcc -O2 btagent.c -o btagent
 */
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
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

#define EVT_CONN_REQUEST         0x04
#define EVT_AUTH_COMPLETE        0x06
#define EVT_PIN_CODE_REQ         0x16
#define EVT_LINK_KEY_REQ         0x17
#define EVT_LINK_KEY_NOTIFY      0x18
#define EVT_IO_CAP_REQ           0x31
#define EVT_USER_CONFIRM_REQ     0x33
#define EVT_SIMPLE_PAIR_COMPLETE 0x36

#define OGF_LINK_CTL           0x01
#define OCF_ACCEPT_CONN        0x0009
#define OCF_LINK_KEY_NEG       0x000c
#define OCF_PIN_REPLY          0x000d
#define OCF_IO_CAP_REPLY       0x002b
#define OCF_USER_CONFIRM_REPLY 0x002c

static int hci = -1;
static const char *pin = "0000";

static void say(const char *fmt, ...)
{
	va_list ap;
	char msg[256];
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char st[16];
	strftime(st, sizeof(st), "%H:%M:%S", &tm);
	printf("%s %s\n", st, msg);
	fflush(stdout);
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
	if (write(hci, pkt, 4 + n) < 0)
		say("не отправился ответ контроллеру");
}

static void mac_str(const unsigned char *raw, char *out)
{
	sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
		raw[5], raw[4], raw[3], raw[2], raw[1], raw[0]);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		pin = argv[1];
	int lock = open("/tmp/.btagent.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
		return 0;                       /* уже работает */
	signal(SIGPIPE, SIG_IGN);

	hci = socket(AF_BLUETOOTH_, SOCK_RAW, BTPROTO_HCI_);
	if (hci < 0) {
		say("нет доступа к HCI");
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
		say("не удалось привязаться к hci0");
		return 1;
	}
	say("жду входящее сопряжение, PIN %s", pin);

	for (;;) {
		unsigned char d[300];
		int r = read(hci, d, sizeof(d));
		if (r < 3 || d[0] != 0x04)
			continue;
		int code = d[1];
		unsigned char *body = d + 3;
		char mac[18];

		if (code == EVT_CONN_REQUEST && r >= 13) {
			mac_str(body, mac);
			say("запрос соединения от %s — принимаю", mac);
			unsigned char p[7];
			memcpy(p, body, 6);
			p[6] = 0x01;            /* остаёмся ведомыми */
			send_cmd(OGF_LINK_CTL, OCF_ACCEPT_CONN, p, 7);
		} else if (code == EVT_LINK_KEY_REQ && r >= 9) {
			mac_str(body, mac);
			say("просят ключ связи для %s — ключа нет", mac);
			send_cmd(OGF_LINK_CTL, OCF_LINK_KEY_NEG, body, 6);
		} else if (code == EVT_PIN_CODE_REQ && r >= 9) {
			mac_str(body, mac);
			say("просят PIN для %s — отдаю %s", mac, pin);
			unsigned char p[23];
			memcpy(p, body, 6);
			int pl = strlen(pin);
			if (pl > 16)
				pl = 16;
			p[6] = (unsigned char)pl;
			memset(p + 7, 0, 16);
			memcpy(p + 7, pin, pl);
			send_cmd(OGF_LINK_CTL, OCF_PIN_REPLY, p, 23);
		} else if (code == EVT_IO_CAP_REQ && r >= 9) {
			mac_str(body, mac);
			say("спрашивают наши возможности ввода (%s)", mac);
			unsigned char p[9];
			memcpy(p, body, 6);
			p[6] = 0x03;            /* NoInputNoOutput */
			p[7] = 0x00;            /* без OOB */
			p[8] = 0x00;            /* без требования MITM */
			send_cmd(OGF_LINK_CTL, OCF_IO_CAP_REPLY, p, 9);
		} else if (code == EVT_USER_CONFIRM_REQ && r >= 9) {
			mac_str(body, mac);
			say("просят подтверждение (%s) — подтверждаю", mac);
			send_cmd(OGF_LINK_CTL, OCF_USER_CONFIRM_REPLY, body, 6);
		} else if (code == EVT_LINK_KEY_NOTIFY && r >= 9) {
			mac_str(body, mac);
			say("получен ключ связи для %s", mac);
		} else if (code == EVT_AUTH_COMPLETE ||
			   code == EVT_SIMPLE_PAIR_COMPLETE) {
			say("сопряжение завершено, код %d", body[0]);
		}
	}
	return 0;
}
