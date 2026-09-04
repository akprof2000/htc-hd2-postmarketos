/* ringtone — звонок HTC HD2 в /dev/msm_pcm_out.
 *
 * Мелодия — фрагмент «Gran Vals» Ф. Тарреги (1902, общественное
 * достояние), он же классический «нокиевский» мотив. Синтез —
 * затухающий синус с лёгкой второй гармоникой; готовый PCM кэшируется
 * в файл, чтобы не считать его при каждом звонке (на этом процессоре
 * синтез заметно дольше самого проигрывания).
 *
 * Управление устройством — той же последовательностью, что в
 * проверенном beep.c. Буферы отдаются только целиком, поэтому хвост
 * дополняется тишиной.
 *
 * Запуск: ringtone [h]   (h — в наушники, иначе динамик)
 * Сборка: gcc -O2 ringtone.c -lm -o ringtone
 */
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUDIO_IOCTL_MAGIC 'a'
#define AUDIO_START         _IOW(AUDIO_IOCTL_MAGIC, 0, unsigned)
#define AUDIO_STOP          _IOW(AUDIO_IOCTL_MAGIC, 1, unsigned)
#define AUDIO_GET_CONFIG    _IOR(AUDIO_IOCTL_MAGIC, 3, unsigned)
#define AUDIO_SET_CONFIG    _IOW(AUDIO_IOCTL_MAGIC, 4, unsigned)
#define AUDIO_SET_VOLUME    _IOW(AUDIO_IOCTL_MAGIC, 10, unsigned)
#define AUDIO_SWITCH_DEVICE _IOW(AUDIO_IOCTL_MAGIC, 32, unsigned)

#define DEV_SPKR_MONO      0x1081513u
#define DEV_HEADSET_STEREO 0x107ac8au

#define RATE 44100
#define CACHE_DIR "/var/lib/phone"
#define CACHE     "/var/lib/phone/ringtone.raw"

struct msm_audio_config {
	uint32_t buffer_size, buffer_count, channel_count, sample_rate,
		 type, unused[3];
};

/* нота в полутонах от ля первой октавы и длительность в восьмушках */
struct note { int semi, beats; };
static const struct note MELODY[] = {
	{ 7, 1}, { 5, 1}, {-3, 2}, {-1, 2},
	{ 4, 1}, { 2, 1}, {-7, 2}, {-5, 2},
	{ 2, 1}, { 0, 1}, {-9, 2}, {-5, 2}, { 0, 6},
};
static const int NOTES = (int)(sizeof(MELODY) / sizeof(MELODY[0]));
static const double UNIT = 0.125;         /* восьмушка, секунды */

/* синтез мелодии; возвращает буфер и его размер в байтах */
static unsigned char *synth(size_t *out_len)
{
	size_t total = 0;
	for (int i = 0; i < NOTES; i++)
		total += (size_t)(RATE * UNIT * MELODY[i].beats);
	size_t bytes = total * 2 * sizeof(int16_t);    /* два канала */
	int16_t *pcm = malloc(bytes);
	if (!pcm)
		return NULL;

	size_t p = 0;
	for (int i = 0; i < NOTES; i++) {
		double f = 440.0 * pow(2.0, MELODY[i].semi / 12.0);
		long n = (long)(RATE * UNIT * MELODY[i].beats);
		for (long t = 0; t < n; t++) {
			double env = exp(-3.0 * t / n);
			if (t < 300)
				env *= t / 300.0;      /* мягкая атака */
			double v = sin(2 * M_PI * f * t / RATE) +
				   0.35 * sin(4 * M_PI * f * t / RATE);
			int16_t s = (int16_t)(11000 * env * v);
			pcm[p++] = s;
			pcm[p++] = s;
		}
	}
	*out_len = bytes;
	return (unsigned char *)pcm;
}

static unsigned char *load(size_t *len)
{
	struct stat st;
	if (stat(CACHE, &st) == 0 && st.st_size > 100000) {
		unsigned char *d = malloc((size_t)st.st_size);
		int fd = d ? open(CACHE, O_RDONLY) : -1;
		if (fd >= 0) {
			ssize_t got = 0, n;
			while (got < st.st_size &&
			       (n = read(fd, d + got, (size_t)(st.st_size - got))) > 0)
				got += n;
			close(fd);
			if (got == st.st_size) {
				*len = (size_t)st.st_size;
				return d;
			}
		}
		free(d);
	}
	unsigned char *d = synth(len);
	if (!d)
		return NULL;
	mkdir(CACHE_DIR, 0755);
	int fd = open(CACHE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		if (write(fd, d, *len) < 0) { /* кэш не критичен */ }
		close(fd);
	}
	return d;
}

int main(int argc, char **argv)
{
	int headset = (argc > 1 && argv[1][0] == 'h');
	size_t len = 0;
	unsigned char *data = load(&len);
	if (!data)
		return 1;

	int ctl = open("/dev/msm_audio_ctl", O_RDWR);
	if (ctl >= 0) {
		uint32_t sw[2] = { headset ? DEV_HEADSET_STEREO : DEV_SPKR_MONO, 0 };
		if (ioctl(ctl, AUDIO_SWITCH_DEVICE, &sw) < 0)
			perror("SWITCH_DEVICE");
		uint32_t v = 100;
		if (ioctl(ctl, AUDIO_SET_VOLUME, &v) < 0)
			perror("SET_VOLUME");
	}

	int fd = open("/dev/msm_pcm_out", O_RDWR);
	if (fd < 0) {
		perror("open msm_pcm_out");
		return 1;
	}
	struct msm_audio_config c;
	if (ioctl(fd, AUDIO_GET_CONFIG, &c) < 0) {
		perror("GET_CONFIG");
		return 1;
	}
	c.channel_count = 2;
	c.sample_rate = RATE;
	if (ioctl(fd, AUDIO_SET_CONFIG, &c) < 0)
		perror("SET_CONFIG");
	if (ioctl(fd, AUDIO_START, 0) < 0) {
		perror("START");
		return 1;
	}

	unsigned char *tail = malloc(c.buffer_size);
	for (size_t i = 0; i < len; i += c.buffer_size) {
		size_t left = len - i;
		if (left >= c.buffer_size) {
			if (write(fd, data + i, c.buffer_size) < 0)
				break;
		} else if (tail) {                 /* хвост дополняем тишиной */
			memcpy(tail, data + i, left);
			memset(tail + left, 0, c.buffer_size - left);
			if (write(fd, tail, c.buffer_size) < 0)
				break;
		}
	}

	free(tail);
	free(data);
	ioctl(fd, AUDIO_STOP, 0);
	close(fd);
	if (ctl >= 0)
		close(ctl);
	return 0;
}
