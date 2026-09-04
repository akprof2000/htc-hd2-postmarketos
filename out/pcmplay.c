/* pcmplay — проигрыватель PCM со стандартного ввода в /dev/msm_pcm_out.
 *
 * Легаси-звук MSM: устройство принимает только целые буферы того
 * размера, который само назвало в GET_CONFIG, поэтому хвост короче
 * буфера дополняется тишиной.
 *
 * Кормить так:
 *     ffmpeg -i файл -f s16le -ac 2 -ar 44100 - | pcmplay [h]
 * h — наушники, иначе динамик. Громкость берётся из /run/phone/vol.
 *
 * Сборка: gcc -O2 pcmplay.c -o pcmplay
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

struct msm_audio_config {
	uint32_t buffer_size, buffer_count, channel_count, sample_rate,
		 type, unused[3];
};

static unsigned volume(void)
{
	int fd = open("/run/phone/vol", O_RDONLY);
	if (fd < 0)
		return 70;
	char b[16] = {0};
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0)
		return 70;
	int v = atoi(b);
	if (v < 0)
		v = 0;
	if (v > 100)
		v = 100;
	return (unsigned)v;
}

int main(int argc, char **argv)
{
	int headset = (argc > 1 && argv[1][0] == 'h');
	uint32_t dev = headset ? DEV_HEADSET_STEREO : DEV_SPKR_MONO;

	int ctl = open("/dev/msm_audio_ctl", O_RDWR);
	if (ctl >= 0) {
		uint32_t sw[2] = { dev, 0 };
		if (ioctl(ctl, AUDIO_SWITCH_DEVICE, &sw) < 0)
			perror("SWITCH_DEVICE");
		uint32_t v = volume();
		if (ioctl(ctl, AUDIO_SET_VOLUME, &v) < 0)
			perror("SET_VOLUME");
	} else {
		perror("open msm_audio_ctl");
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
	c.sample_rate = 44100;
	if (ioctl(fd, AUDIO_SET_CONFIG, &c) < 0)
		perror("SET_CONFIG");
	if (ioctl(fd, AUDIO_START, 0) < 0) {
		perror("START");
		return 1;
	}

	unsigned char *buf = malloc(c.buffer_size);
	if (!buf) {
		ioctl(fd, AUDIO_STOP, 0);
		return 1;
	}
	for (;;) {
		size_t got = 0;
		while (got < c.buffer_size) {
			ssize_t n = read(0, buf + got, c.buffer_size - got);
			if (n <= 0)
				break;
			got += (size_t)n;
		}
		if (got == 0)
			break;
		if (got < c.buffer_size)   /* хвост — дополняем тишиной */
			memset(buf + got, 0, c.buffer_size - got);
		if (write(fd, buf, c.buffer_size) < 0) {
			perror("write");
			break;
		}
		if (got < c.buffer_size)
			break;
	}

	free(buf);
	ioctl(fd, AUDIO_STOP, 0);
	close(fd);
	if (ctl >= 0)
		close(ctl);
	return 0;
}
