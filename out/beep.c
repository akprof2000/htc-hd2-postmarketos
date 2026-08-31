/* Проба звука через легаси-интерфейс MSM (/dev/msm_pcm_out) ядра 3.0 htc-leo.
 *
 *   beep                — динамик, громкость 100
 *   beep h              — наушники
 *   beep s 60           — динамик, громкость 60 (шкала 0..100, выше отвергается)
 *   beep s 100 r        — плюс перечитать калибровку ACDB
 *
 * Калибровку перечитывают ОДИН раз за загрузку: повторные вызовы ломают уже
 * настроенный тракт. Поэтому по умолчанию не трогаем.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <sys/ioctl.h>

#define AUDIO_IOCTL_MAGIC 'a'
#define AUDIO_START         _IOW(AUDIO_IOCTL_MAGIC, 0, unsigned)
#define AUDIO_STOP          _IOW(AUDIO_IOCTL_MAGIC, 1, unsigned)
#define AUDIO_GET_CONFIG    _IOR(AUDIO_IOCTL_MAGIC, 3, unsigned)
#define AUDIO_SET_CONFIG    _IOW(AUDIO_IOCTL_MAGIC, 4, unsigned)
#define AUDIO_SET_VOLUME    _IOW(AUDIO_IOCTL_MAGIC, 10, unsigned)
#define AUDIO_SWITCH_DEVICE _IOW(AUDIO_IOCTL_MAGIC, 32, unsigned)
#define AUDIO_REINIT_ACDB   _IOW(AUDIO_IOCTL_MAGIC, 39, unsigned)

#define DEV_SPKR_MONO      0x1081513u
#define DEV_HEADSET_STEREO 0x107ac8au

struct msm_audio_config {
	uint32_t buffer_size, buffer_count, channel_count, sample_rate,
		 type, unused[3];
};

int main(int argc, char **argv) {
	int headset = (argc > 1 && argv[1][0] == 'h');
	uint32_t dev = headset ? DEV_HEADSET_STEREO : DEV_SPKR_MONO;
	uint32_t vol = (argc > 2) ? (uint32_t)atoi(argv[2]) : 100;
	int reinit  = (argc > 3 && argv[3][0] == 'r');

	int ctl = open("/dev/msm_audio_ctl", O_RDWR);
	if (ctl >= 0) {
		if (reinit) {
			char fn[64];
			memset(fn, 0, sizeof(fn));
			strcpy(fn, "default.acdb");
			if (ioctl(ctl, AUDIO_REINIT_ACDB, fn) < 0)
				perror("REINIT_ACDB");
			else
				printf("калибровка перечитана\n");
		}
		uint32_t sw[2] = { dev, 0 };
		if (ioctl(ctl, AUDIO_SWITCH_DEVICE, &sw) < 0)
			perror("SWITCH_DEVICE");
		if (ioctl(ctl, AUDIO_SET_VOLUME, &vol) < 0)
			perror("SET_VOLUME");
	} else {
		perror("open msm_audio_ctl");
	}

	int fd = open("/dev/msm_pcm_out", O_RDWR);
	if (fd < 0) { perror("open msm_pcm_out"); return 1; }

	struct msm_audio_config c;
	if (ioctl(fd, AUDIO_GET_CONFIG, &c) < 0) { perror("GET_CONFIG"); return 1; }
	printf("%s, громкость %u, буфер %u x%u\n",
	       headset ? "наушники" : "динамик", vol, c.buffer_size, c.buffer_count);

	c.channel_count = 2;
	c.sample_rate = 44100;
	if (ioctl(fd, AUDIO_SET_CONFIG, &c) < 0) perror("SET_CONFIG");
	if (ioctl(fd, AUDIO_START, 0) < 0) { perror("START"); return 1; }

	int16_t *buf = malloc(c.buffer_size);
	unsigned frames = c.buffer_size / 4, t = 0, total = 44100 * 2;
	while (t < total) {
		unsigned i;
		for (i = 0; i < frames; i++, t++) {
			int16_t s = (int16_t)(12000.0 *
					sin(2 * 3.14159265 * 660.0 * t / 44100.0));
			buf[2 * i] = s;
			buf[2 * i + 1] = s;
		}
		if (write(fd, buf, c.buffer_size) < 0) { perror("write"); break; }
	}

	ioctl(fd, AUDIO_STOP, 0);
	close(fd);
	if (ctl >= 0) close(ctl);
	printf("готово\n");
	return 0;
}
