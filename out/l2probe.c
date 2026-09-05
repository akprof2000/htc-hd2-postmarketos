/* l2probe — зонд L2CAP-соединения: где и когда оно рвётся.
 *
 * Ставит таймаут отправки ПРЯМЫМ системным вызовом со старой 8-байтной
 * структурой timeval (в обход musl, у которого time_t 64-битный),
 * читает его обратно тем же способом, потом соединяется с PSM и меряет
 * время до результата.
 *
 *     l2probe <MAC> [PSM]        по умолчанию PSM 1 (SDP)
 *
 * Сборка: gcc -O2 l2probe.c -o l2probe
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

struct sockaddr_l2_ {
	unsigned short family, psm;
	unsigned char bdaddr[6];
	unsigned short cid;
};

static double now_s(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
	unsigned v[6];
	if (argc < 2 || sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &v[0], &v[1],
			       &v[2], &v[3], &v[4], &v[5]) != 6) {
		printf("l2probe <MAC> [PSM]\n");
		return 2;
	}
	int psm = argc > 2 ? atoi(argv[2]) : 1;
	int s = socket(31, SOCK_SEQPACKET, 0);
	if (s < 0) {
		printf("socket: %s\n", strerror(errno));
		return 1;
	}
	struct sockaddr_l2_ a;
	memset(&a, 0, sizeof(a));
	a.family = 31;
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0)
		printf("bind: %s\n", strerror(errno));

	/* старый timeval: два 32-битных поля */
	struct { int sec, usec; } old = {40, 0}, back = {0, 0};
	long rc = syscall(SYS_setsockopt, s, SOL_SOCKET, 21 /*SO_SNDTIMEO*/,
			  &old, sizeof(old));
	printf("setsockopt(SO_SNDTIMEO, 40 с) прямым вызовом: %ld %s\n", rc,
	       rc ? strerror(errno) : "");
	socklen_t bl = sizeof(back);
	rc = syscall(SYS_getsockopt, s, SOL_SOCKET, 21, &back, &bl);
	printf("читается обратно: %d с (rc %ld, len %d)\n", back.sec, rc, bl);

	memset(&a, 0, sizeof(a));
	a.family = 31;
	a.psm = psm;
	for (int i = 0; i < 6; i++)
		a.bdaddr[i] = (unsigned char)v[5 - i];
	double t0 = now_s();
	int r = connect(s, (struct sockaddr *)&a, sizeof(a));
	int e = errno;
	printf("connect(PSM %d): %d %s через %.2f с\n", psm, r,
	       r ? strerror(e) : "успех", now_s() - t0);
	if (r < 0 && (e == EINPROGRESS || e == EAGAIN)) {
		for (int i = 0; i < 400; i++) {
			struct pollfd pf = {s, POLLOUT, 0};
			int pr = poll(&pf, 1, 100);
			if (pr > 0) {
				int err = 0;
				socklen_t el = sizeof(err);
				getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
				printf("poll: события 0x%x, SO_ERROR=%d (%s) через %.2f с\n",
				       pf.revents, err, strerror(err), now_s() - t0);
				break;
			}
		}
	}
	close(s);
	return 0;
}
