/* Голосовой тракт разговора на HTC HD2 (ядро 3.0, qdsp6_1550).
 *
 * Сигнализация вызова идёт AT-командами через /dev/smd0, но звук разговора
 * поднимается отдельно: ядро должно запустить голосовой путь DSP
 * (AUDIO_START_VOICE) и задать устройства приёма/передачи.
 *
 * voice on  [h] — включить (h = гарнитура, иначе динамик/микрофон)
 * voice off     — выключить
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define AUDIO_IOCTL_MAGIC 'a'
#define AUDIO_SWITCH_DEVICE _IOW(AUDIO_IOCTL_MAGIC, 32, unsigned)
#define AUDIO_SET_MUTE      _IOW(AUDIO_IOCTL_MAGIC, 33, unsigned)
#define AUDIO_START_VOICE   _IOW(AUDIO_IOCTL_MAGIC, 35, unsigned)
#define AUDIO_STOP_VOICE    _IOW(AUDIO_IOCTL_MAGIC, 36, unsigned)
#define AUDIO_SET_VOLUME    _IOW(AUDIO_IOCTL_MAGIC, 10, unsigned)

/* идентификаторы устройств ADSP */
#define DEV_HANDSET_SPKR  0x1081511u   /* динамик трубки */
#define DEV_HANDSET_MIC   0x1081512u
#define DEV_HEADSET_SPKR  0x107ac8au   /* гарнитура */
#define DEV_HEADSET_MIC   0x1081510u

int main(int argc, char **argv) {
	int headset = (argc > 2 && argv[2][0] == 'h');
	int on = (argc > 1 && strcmp(argv[1], "on") == 0);

	int ctl = open("/dev/msm_audio_ctl", O_RDWR);
	if (ctl < 0) { perror("open msm_audio_ctl"); return 1; }

	if (!on) {
		if (ioctl(ctl, AUDIO_STOP_VOICE, 0) < 0) perror("STOP_VOICE");
		else printf("голосовой тракт выключен\n");
		close(ctl);
		return 0;
	}

	/* маршрут: приём и передача */
	uint32_t rx[2] = { headset ? DEV_HEADSET_SPKR : DEV_HANDSET_SPKR, 0 };
	uint32_t tx[2] = { headset ? DEV_HEADSET_MIC  : DEV_HANDSET_MIC,  0 };
	if (ioctl(ctl, AUDIO_SWITCH_DEVICE, &rx) < 0) perror("SWITCH rx");
	if (ioctl(ctl, AUDIO_SWITCH_DEVICE, &tx) < 0) perror("SWITCH tx");

	/* запуск голосового пути DSP */
	uint32_t acdb[2] = { 0, 0 };
	if (ioctl(ctl, AUDIO_START_VOICE, &acdb) < 0) {
		perror("START_VOICE");
	} else {
		printf("голосовой тракт запущен (%s)\n",
		       headset ? "гарнитура" : "трубка");
	}

	uint32_t vol = 90;
	if (ioctl(ctl, AUDIO_SET_VOLUME, &vol) < 0) perror("SET_VOLUME");
	uint32_t mute = 0;
	if (ioctl(ctl, AUDIO_SET_MUTE, &mute) < 0) perror("SET_MUTE");

	close(ctl);
	return 0;
}
