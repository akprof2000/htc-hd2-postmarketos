/* btsend — передача файла по Bluetooth (OBEX Push) с HTC HD2.
 *
 * Готовых obexftp/obexpushd в системе нет, поэтому говорим по OBEX
 * сами: RFCOMM-сокет -> CONNECT -> PUT кусками -> DISCONNECT.
 *
 *     btsend <MAC> <файл> [канал]
 *
 * Канал ищем через sdptool; если не вышло — перебираем 9, 12, 4, 10, 3
 * (обычные каналы OBEX Push у телефонов).
 *
 * Сборка: gcc -O2 btsend.c -o btsend
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#define AF_BLUETOOTH_  31
#define BTPROTO_RFCOMM 3

/* sockaddr_rc из BlueZ: семейство, адрес (6 байт), канал */
struct sockaddr_rc_ {
	unsigned short family;
	unsigned char  bdaddr[6];
	unsigned char  channel;
	unsigned char  pad;
};

#define OBEX_CONNECT    0x80
#define OBEX_DISCONNECT 0x81
#define OBEX_PUT_FINAL  0x82
#define OBEX_PUT        0x02
#define HI_NAME     0x01
#define HI_LENGTH   0xc3
#define HI_BODY     0x48
#define HI_BODY_END 0x49
#define MTU 4096

static const char *resp_name(int code)
{
	switch (code) {
	case 0xa0: return "OK";
	case 0xc0: return "запрос отклонён";
	case 0xc3: return "доступ запрещён";
	case 0xc6: return "нет места";
	case 0xd0: return "внутренняя ошибка";
	default:   return "неизвестный ответ";
	}
}

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

/* канал OBEX Push по SDP; 0 — не нашли */
static int find_channel(const char *mac)
{
	char cmd[160];
	snprintf(cmd, sizeof(cmd),
		 "timeout 25 sdptool search --bdaddr %s OPUSH 2>/dev/null", mac);
	FILE *f = popen(cmd, "r");
	if (!f)
		return 0;
	char ln[256];
	int want = 0, ch = 0;
	while (fgets(ln, sizeof(ln), f)) {
		if (strstr(ln, "RFCOMM"))
			want = 1;
		else if (want) {
			char *p = strstr(ln, "Channel:");
			if (p) {
				ch = atoi(p + 8);
				break;
			}
		}
	}
	pclose(f);
	return ch;
}

/* соединение с ограничением по времени: обычный connect на RFCOMM ждёт
 * страничный таймаут контроллера, и перебор шести каналов растягивается
 * на минуты */
static int connect_timed(int s, struct sockaddr *a, int alen, int secs)
{
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int r = connect(s, a, alen);
    if (r == 0) {
        fcntl(s, F_SETFL, fl);
        return 1;
    }
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    struct timeval tv = {secs, 0};
    if (select(s + 1, NULL, &w, NULL, &tv) <= 0) {
        fcntl(s, F_SETFL, fl);
        return 0;
    }
    int err = 0;
    socklen_t el = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
    fcntl(s, F_SETFL, fl);
    return err == 0;
}

static int recv_all(int s, unsigned char *b, int n)
{
	int got = 0;
	while (got < n) {
		int r = read(s, b + got, n - got);
		if (r <= 0)
			return 0;
		got += r;
	}
	return 1;
}

/* пакет OBEX: код + длина; тело кладём в buf */
static int recv_pkt(int s, int *code, unsigned char *buf, int bufsz, int *blen)
{
	unsigned char head[3];
	if (!recv_all(s, head, 3))
		return 0;
	*code = head[0];
	int len = (head[1] << 8) | head[2];
	int body = len - 3;
	if (body < 0 || body > bufsz)
		return 0;
	if (body && !recv_all(s, buf, body))
		return 0;
	*blen = body;
	return 1;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		printf("использование: btsend <MAC> <файл> [канал]\n");
		return 2;
	}
	const char *mac = argv[1], *path = argv[2];
	unsigned char addr[6];
	if (!parse_mac(mac, addr)) {
		printf("непонятный адрес: %s\n", mac);
		return 2;
	}

	/* файл целиком в память: снимки с этого телефона — единицы мегабайт */
	struct stat st;
	if (stat(path, &st) < 0) {
		printf("нет файла: %s\n", path);
		return 2;
	}
	unsigned char *data = malloc((size_t)st.st_size);
	int fd = data ? open(path, O_RDONLY) : -1;
	if (fd < 0 || !recv_all(fd, data, (int)st.st_size)) {
		printf("не прочитался файл: %s\n", path);
		return 2;
	}
	close(fd);
	long dlen = (long)st.st_size;
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;

	int channels[8], nch = 0;
	if (argc > 3)
		channels[nch++] = atoi(argv[3]);
	int found = find_channel(mac);
	if (found)
		channels[nch++] = found;
	int def[5] = {9, 12, 4, 10, 3};
	for (int i = 0; i < 5 && nch < 8; i++)
		channels[nch++] = def[i];

	char last_err[128] = "нет ответа";
	for (int ci = 0; ci < nch; ci++) {
		int ch = channels[ci];
		if (ch <= 0)
			continue;
		int s = socket(AF_BLUETOOTH_, SOCK_STREAM, BTPROTO_RFCOMM);
		if (s < 0)
			continue;
		struct timeval tv = {25, 0};
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		struct sockaddr_rc_ a;
		memset(&a, 0, sizeof(a));
		a.family = AF_BLUETOOTH_;
		memcpy(a.bdaddr, addr, 6);
		a.channel = (unsigned char)ch;
		if (!connect_timed(s, (struct sockaddr *)&a, sizeof(a), 12)) {
			snprintf(last_err, sizeof(last_err),
				 "канал %d: соединение не открылось", ch);
			close(s);
			continue;
		}
		printf("соединение по каналу %d\n", ch);
		fflush(stdout);

		/* CONNECT: версия 1.0, флаги 0, MTU */
		/* с запасом: в первый пакет кроме куска тела уходят ещё и
		 * заголовки (имя файла в UTF-16 плюс длина) */
		unsigned char pkt[MTU + 1024];
		pkt[0] = OBEX_CONNECT; pkt[1] = 0; pkt[2] = 7;
		pkt[3] = 0x10; pkt[4] = 0x00;
		pkt[5] = MTU >> 8; pkt[6] = MTU & 0xff;
		int code = 0, blen = 0;
		unsigned char body[512];
		if (write(s, pkt, 7) != 7 ||
		    !recv_pkt(s, &code, body, sizeof(body), &blen) ||
		    code != 0xa0) {
			snprintf(last_err, sizeof(last_err), "OBEX CONNECT: %s",
				 resp_name(code));
			close(s);
			continue;
		}
		int peer_mtu = blen >= 4 ? ((body[2] << 8) | body[3]) : MTU;
		int chunk = (peer_mtu < MTU ? peer_mtu : MTU) - 64;
		if (chunk < 64)
			chunk = 64;
		if (chunk > (int)sizeof(pkt) - 1024)
			chunk = (int)sizeof(pkt) - 1024;

		/* заголовки: имя в UTF-16BE с нулём и полная длина */
		unsigned char hdr[512];
		int hl = 0;
		int nl = (int)strlen(base);
		int uni = (nl + 1) * 2;
		hdr[hl++] = HI_NAME;
		hdr[hl++] = (3 + uni) >> 8;
		hdr[hl++] = (3 + uni) & 0xff;
		for (int i = 0; i < nl; i++) {   /* имя латиницей/цифрами */
			hdr[hl++] = 0;
			hdr[hl++] = (unsigned char)base[i];
		}
		hdr[hl++] = 0; hdr[hl++] = 0;
		hdr[hl++] = HI_LENGTH;
		hdr[hl++] = (dlen >> 24) & 0xff;
		hdr[hl++] = (dlen >> 16) & 0xff;
		hdr[hl++] = (dlen >> 8) & 0xff;
		hdr[hl++] = dlen & 0xff;

		long off = 0;
		int first = 1, ok = 1;
		while (off < dlen) {
			int part = (int)(dlen - off);
			if (part > chunk)
				part = chunk;
			int last = (off + part >= dlen);
			int p = 3;
			if (first) {
				memcpy(pkt + p, hdr, hl);
				p += hl;
				first = 0;
			}
			pkt[p++] = last ? HI_BODY_END : HI_BODY;
			pkt[p++] = (3 + part) >> 8;
			pkt[p++] = (3 + part) & 0xff;
			memcpy(pkt + p, data + off, part);
			p += part;
			pkt[0] = last ? OBEX_PUT_FINAL : OBEX_PUT;
			pkt[1] = p >> 8;
			pkt[2] = p & 0xff;
			if (write(s, pkt, p) != p ||
			    !recv_pkt(s, &code, body, sizeof(body), &blen)) {
				snprintf(last_err, sizeof(last_err),
					 "обрыв при передаче");
				ok = 0;
				break;
			}
			if (code != 0x90 && code != 0xa0) {  /* CONTINUE / OK */
				snprintf(last_err, sizeof(last_err),
					 "передача: %s", resp_name(code));
				ok = 0;
				break;
			}
			off += part;
			printf("отправлено %ld%%\n", off * 100 / (dlen ? dlen : 1));
			fflush(stdout);
		}
		if (ok) {
			unsigned char dis[3] = {OBEX_DISCONNECT, 0, 3};
			if (write(s, dis, 3) < 0) { }
			close(s);
			printf("ФАЙЛ ПЕРЕДАН: %s (%ld байт)\n", base, dlen);
			free(data);
			return 0;
		}
		close(s);
	}
	printf("не удалось: %s\n", last_err);
	free(data);
	return 1;
}
