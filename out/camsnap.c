/* RAW-снимок с камеры HTC HD2 (s5k3e2fx + VFE8x, msm_camera v1).
 *
 * Путь данных: сенсор (Bayer) -> CAMIF -> AXI (обход обработки VFE) ->
 * pmem-буфер -> файл. Проявка снимка — отдельно, Pillow-ом.
 *
 * Сборка: gcc -O2 -static camsnap.c (нужны заголовки ядра msm_camera.h,
 * msm_vfe8x.h рядом).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <time.h>

#define __user
#include "msm_camera.h"
#include "msm_vfe8x.h"

#define W 1296
#define H 972
/* ядро принудительно ставит 8-бит RAW: байт на пиксель */
#define RAWLEN (W * H)

static int cfg, ctrl;

static int vfe_cmd(int id, void *val, int len)
{
	struct msm_vfe_command_8k vc = { id, len, val };
	struct msm_vfe_cfg_cmd cc = { CMD_GENERAL, sizeof(vc), &vc };
	if (ioctl(cfg, MSM_CAM_IOCTL_CONFIG_VFE, &cc) < 0) {
		printf("VFE cmd %d: %s\n", id, strerror(errno));
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	/* camsnap [gain] [line] [видео_секунд] — экспозиция сенсора;
	 * третий аргумент включает видеорежим: кадры в /tmp/vid/fNNN.raw */
	int gain = argc > 1 ? atoi(argv[1]) : 120;
	int line = argc > 2 ? atoi(argv[2]) : 1500;
	int vidsec = argc > 3 ? atoi(argv[3]) : 0;
	/* число кадров за один запуск VFE: 1 — обычный снимок; больше —
	 * VFE продолжает перезаписывать тот же буфер (живое превью)
	 * без повторных START/STOP, которые роняют ядро */
	int shots = argc > 4 ? atoi(argv[4]) : 1;
	setvbuf(stdout, NULL, _IONBF, 0);

	/* камера строго однопользовательская: второй экземпляр стека
	 * параллельно с первым роняет ядро */
	int lockfd = open("/run/.camsnap.lock", O_CREAT | O_RDWR, 0644);
	if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
		printf("камера занята другим процессом\n");
		return 2;
	}

	ctrl = open("/dev/msm_camera/control0", O_RDWR);
	cfg = open("/dev/msm_camera/config0", O_RDWR);
	if (ctrl < 0 || cfg < 0) {
		printf("open: %s\n", strerror(errno));
		return 1;
	}
	printf("камера открыта\n");

	/* pmem-буфер под сырой кадр */
	int pfd = open("/dev/pmem_adsp", O_RDWR);
	if (pfd < 0) { printf("pmem: %s\n", strerror(errno)); return 1; }
	size_t fsz = (RAWLEN + 4095) & ~4095u;
	size_t blen = fsz * 3;            /* VFE пишет 3 кадровых слота */
	uint8_t *buf = mmap(NULL, blen, PROT_READ | PROT_WRITE, MAP_SHARED,
			    pfd, 0);
	if (buf == MAP_FAILED) {
		printf("mmap %zu: %s\n", blen, strerror(errno));
		return 1;
	}
	memset(buf, 0xA5, blen);   /* маркер: увидим, что перезаписано */
	printf("pmem %zu байт готов\n", blen);

	/* один буфер, но Y и CbCr каналы разнесены по половинам —
	 * так видно, какой канал что пишет */
	struct msm_pmem_info pi;
	memset(&pi, 0, sizeof(pi));
	pi.type = MSM_PMEM_RAW_MAINIMG;
	pi.fd = pfd;
	pi.vaddr = buf;
	pi.offset = 0;
	pi.len = fsz * 2;
	pi.y_off = 0;
	pi.cbcr_off = fsz;
	pi.vfe_can_write = 1;
	if (ioctl(cfg, MSM_CAM_IOCTL_REGISTER_PMEM, &pi) < 0) {
		printf("REGISTER_PMEM: %s\n", strerror(errno));
		return 1;
	}
	printf("буфер Y|CbCr зарегистрирован\n");

	/* сенсор в полнокадровый режим */
	struct sensor_cfg_data sc;
	memset(&sc, 0, sizeof(sc));
	sc.cfgtype = CFG_SET_MODE;
	sc.mode = SENSOR_PREVIEW_MODE;
	sc.rs = 0;                        /* S_QTR_SIZE 1296x972 */
	if (ioctl(cfg, MSM_CAM_IOCTL_SENSOR_IO_CFG, &sc) < 0)
		printf("SENSOR SET_MODE: %s (продолжаем)\n", strerror(errno));
	else
		printf("сенсор: полнокадровый режим\n");
	sleep(1);          /* матрице нужно устояться, иначе VFE виснет */

	/* выдержка/усиление: без них сенсор снимает на минимуме — темно */
	memset(&sc, 0, sizeof(sc));
	sc.cfgtype = CFG_SET_PICT_EXP_GAIN;
	sc.cfg.exp_gain.gain = gain;
	sc.cfg.exp_gain.line = line;
	if (ioctl(cfg, MSM_CAM_IOCTL_SENSOR_IO_CFG, &sc) < 0)
		printf("EXP_GAIN: %s (продолжаем)\n", strerror(errno));
	else
		printf("экспозиция задана\n");
	sleep(1);

	struct camera_enable_cmd en;
	memset(&en, 0, sizeof(en));
	strcpy(en.name, "vfe");
	if (ioctl(cfg, MSM_CAM_IOCTL_ENABLE_VFE, &en) < 0)
		printf("ENABLE_VFE: %s\n", strerror(errno));

	vfe_cmd(VFE_CMD_ID_RESET, NULL, 0);
	usleep(100000);

	/* CAMIF: окно = весь кадр сенсора */
	struct vfe_cmd_camif_config cam;
	memset(&cam, 0, sizeof(cam));
	cam.camifConfig.syncMode = VFE_CAMIF_SYNC_MODE_APS;
	/* полная строка/кадр сенсора С ГАШЕНИЕМ (регистры 0x342/0x340
	 * превью-режима s5k3e2fx), окно — активная область */
	cam.frame.pixelsPerLine = 0x0aac;   /* 2732 */
	cam.frame.linesPerFrame = 0x03e2;   /* 994  */
	cam.window.firstpixel = 0;
	cam.window.lastpixel = W - 1;
	cam.window.firstline = 0;
	cam.window.lastline = H - 1;
	if (vfe_cmd(VFE_CMD_ID_CAMIF_CONFIG, &cam, sizeof(cam)) == 0)
		printf("CAMIF настроен %dx%d\n", W, H);

	/* AXI: сырой поток CAMIF -> память через Output2 */
	struct vfe_cmd_axi_output_config ax;
	memset(&ax, 0, sizeof(ax));
	ax.burstLength = VFE_AXI_BURST_LENGTH_IS_8;
	ax.outputMode = VFE_AXI_OUTPUT_MODE_CAMIFToAXIViaOutput2;
	ax.outputDataSize = VFE_RAW_PIXEL_DATA_SIZE_10BIT;
	ax.output2.fragmentCount = 1;
	/* размеры и шаг — в 64-битных словах шины (8-бит: 8 пикс/слово);
	 * поток идёт только через CbCr-канал Output2 */
	ax.output2.outputY.imageWidth = W;      /* в пикселях */
	ax.output2.outputY.imageHeight = H;
	ax.output2.outputY.outRowCount = H;
	ax.output2.outputY.outRowIncrement = W;
	ax.output2.outputCbcr = ax.output2.outputY;
	/* двойная обёртка: cfg_cmd -> command_8k -> axio */
	struct msm_vfe_command_8k axv = { 0, sizeof(ax), &ax };
	struct msm_vfe_cfg_cmd axc = { CMD_RAW_PICT_AXI_CFG, sizeof(axv),
				       &axv };
	if (ioctl(cfg, MSM_CAM_IOCTL_AXI_CONFIG, &axc) < 0)
		printf("AXI_CONFIG: %s\n", strerror(errno));
	else
		printf("AXI RAW настроен\n");

	/* пуск */
	struct vfe_cmd_start st;
	memset(&st, 0, sizeof(st));
	st.inputSource = VFE_START_INPUT_SOURCE_CAMIF;
	/* ТОЛЬКО SNAPSHOT: непрерывный режим роняет ядро — драйверу нужен
	 * возврат буферов (ACK кадров), которого мы не делаем. */
	st.operationMode = VFE_START_OPERATION_MODE_SNAPSHOT;
	st.snapshotCount = shots;
	st.pixel = VFE_BAYER_GRGRGR;
	if (vfe_cmd(VFE_CMD_ID_START, &st, sizeof(st)) == 0)
		printf("VFE START (снимок)\n");

	if (vidsec < 0) {
		/* превью: бесконечно обновляем один файл, пока живём.
		 * Пишем во временный и переименовываем — читатель никогда
		 * не увидит полукадр. */
		int limit = -vidsec;
		time_t t0 = time(NULL);
		printf("превью запущено\n");
		while (time(NULL) - t0 < limit) {
			/* уменьшаем прямо здесь: каждый 4-й пиксель,
			 * 324x243 = 78 КБ вместо 1.2 МБ — телефону легче */
			static uint8_t small[(W / 4) * (H / 4)];
			const uint8_t *src = buf + fsz;
			for (int y = 0; y < H / 4; y++)
				for (int x = 0; x < W / 4; x++)
					small[y * (W / 4) + x] =
						src[(y * 4) * W + x * 4];
			/* контрольная сумма кадра в журнал: видно,
			 * обновляется ли буфер или стоит на месте */
			unsigned sum = 0;
			for (size_t i = 0; i < sizeof(small); i += 97)
				sum = sum * 31 + small[i];
			printf("кадр %08x\n", sum);
			FILE *f = fopen("/tmp/preview.tmp", "wb");
			if (f) {
				fwrite(small, 1, sizeof(small), f);
				fclose(f);
				rename("/tmp/preview.tmp", "/tmp/preview.raw");
			}
			usleep(500000);
		}
		printf("превью завершено\n");
	}
	if (vidsec > 0) {
		/* видео: копируем живой буфер каждые 400 мс */
		system("mkdir -p /tmp/vid; rm -f /tmp/vid/f*.raw");
		int n = 0, total = vidsec * 1000 / 400;
		sleep(1);                       /* экспозиция устаканилась */
		for (; n < total && n < 120; n++) {
			char name[40];
			sprintf(name, "/tmp/vid/f%03d.raw", n);
			FILE *f = fopen(name, "wb");
			if (f) {
				fwrite(buf + fsz, 1, RAWLEN, f);
				fclose(f);
			}
			usleep(400000);
		}
		printf("видео: %d кадров в /tmp/vid\n", n);
	}
	sleep(2);

	/* заморозить кадр перед чтением */
	if (vfe_cmd(VFE_CMD_ID_STOP, NULL, 0) == 0)
		printf("VFE STOP\n");
	usleep(300000);

	/* сколько буфера перезаписано? */
	size_t marked = 0;
	for (size_t i = 0; i < blen; i += 4096)
		if (buf[i] == 0xA5 && buf[i + 1] == 0xA5)
			marked++;
	printf("кадр: %zu/%zu страниц осталось с маркером\n",
	       marked, blen / 4096);

	for (int i = 0; i < 3; i++) {
		char name[32];
		sprintf(name, "/tmp/raw%d.bin", i);
		FILE *f = fopen(name, "wb");
		if (f) {
			fwrite(buf + i * fsz, 1, RAWLEN, f);
			fclose(f);
		}
	}
	printf("3 кадра сохранены: /tmp/raw{0,1,2}.bin\n");

	/* без DISABLE_VFE: он роняет ядро */
	close(cfg);
	close(ctrl);
	printf("готово\n");
	return 0;
}
