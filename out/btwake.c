/* btwake — разговор с Bluetooth-чипом напрямую через порт.
 *
 * Нужен, когда неудачная заливка прошивки оставила чип в режиме
 * загрузки: hciattach в этом состоянии говорит «Failed to reset chip,
 * invalid HCI event» и сдаётся, а чип на самом деле жив и отвечает —
 * просто не тем, чего ждут. Здесь мы шлём ему HCI Reset сами и
 * показываем всё, что он ответил, байт за байтом.
 *
 *     btwake [устройство]      по умолчанию /dev/ttyHS0
 *
 * Сборка: gcc -O2 btwake.c -o btwake
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/time.h>

static void dump(const unsigned char *b, int n, const char *what)
{
	printf("%s (%d байт):", what, n);
	for (int i = 0; i < n; i++)
		printf(" %02x", b[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/ttyHS0";
	int fd = open(dev, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		printf("не открылся %s\n", dev);
		return 1;
	}
	/* Скорость порта могла остаться не той: неудачная заливка иногда
	 * оставляет чип на повышенной скорости, и тогда обычные 115200
	 * дают мусор — именно его hciattach и называет «invalid HCI
	 * event». Поэтому перебираем ходовые скорости. */
	static const struct { speed_t code; const char *name; } SPEEDS[] = {
		{B115200, "115200"}, {B230400, "230400"}, {B460800, "460800"},
		{B921600, "921600"}, {B3000000, "3000000"}, {B0, NULL}
	};
	struct termios t;
	tcgetattr(fd, &t);
	cfmakeraw(&t);
	cfsetispeed(&t, B115200);
	cfsetospeed(&t, B115200);
	t.c_cflag |= CRTSCTS | CLOCAL | CREAD;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 10;              /* 1 секунда на чтение */
	tcsetattr(fd, TCSANOW, &t);
	tcflush(fd, TCIOFLUSH);

	/* сначала слушаем: вдруг чип сам что-то договаривает */
	unsigned char b[256];
	int n = read(fd, b, sizeof(b));
	if (n > 0)
		dump(b, n, "чип говорил до нас");

	static const unsigned char reset[] = {0x01, 0x03, 0x0c, 0x00};
	for (int si = 0; SPEEDS[si].name; si++) {
	cfsetispeed(&t, SPEEDS[si].code);
	cfsetospeed(&t, SPEEDS[si].code);
	tcsetattr(fd, TCSANOW, &t);
	printf("-- скорость %s\n", SPEEDS[si].name);
	for (int try = 1; try <= 2; try++) {
		tcflush(fd, TCIFLUSH);
		if (write(fd, reset, sizeof(reset)) != sizeof(reset)) {
			printf("не удалось записать в порт\n");
			return 1;
		}
		printf("попытка %d: послал HCI Reset\n", try);
		fflush(stdout);
		n = read(fd, b, sizeof(b));
		if (n > 0) {
			dump(b, n, "ответ");
			/* ждём событие 0x04 0x0e ... 0x03 0x0c 0x00 */
			if (n >= 7 && b[0] == 0x04 && b[1] == 0x0e &&
			    b[4] == 0x03 && b[5] == 0x0c && b[6] == 0x00) {
				printf("чип вернулся в обычный режим\n");
				close(fd);
				return 0;
			}
		} else {
			printf("ответа нет\n");
		}
		usleep(300000);
	}
	}
	close(fd);
	printf("чип не отозвался как надо\n");
	return 1;
}
