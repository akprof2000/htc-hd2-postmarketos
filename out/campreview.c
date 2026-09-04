/* Живое превью камеры HTC HD2 — с ВОЗВРАТОМ буферов драйверу.
 *
 * Прошлые попытки роняли ядро потому, что программа не возвращала кадры:
 * драйвер выдаёт кадр в очередь (GETFRAME), и обязан получить его назад
 * (RELEASE_FRAME_BUFFER). Без возврата видеопроцессор через пару кадров
 * пишет в буфер, который драйвер уже считает свободным — паника.
 *
 * Здесь: камера открывается ОДИН раз, регистрируются три буфера,
 * запускается непрерывный режим, и на каждый кадр шлётся возврат.
 *
 *   campreview [секунд] [gain] [line] [запись_секунд]
 * При записи полные кадры дополнительно пишутся в /tmp/vid/fNNN.raw —
 * видео берётся из ЖИВОГО потока, поэтому оно действительно движется,
 * а превью в это время продолжает работать.
 * Кадр (уменьшённый 324x243) пишется в /tmp/preview.raw.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/file.h>

#define __user
#include "msm_camera.h"
#include "msm_vfe8x.h"

#define W 1296
#define H 972
#define RAWLEN (W * H)
#define NBUF 3

static int cfg, ctrl, frm;
static volatile int stop_now;

static void on_sig(int s) { (void)s; stop_now = 1; }

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
	int secs = argc > 1 ? atoi(argv[1]) : 30;
	int gain = argc > 2 ? atoi(argv[2]) : 60;
	int line = argc > 3 ? atoi(argv[3]) : 900;
	int recsec = argc > 4 ? atoi(argv[4]) : 0;   /* запись видео */
	/* 5-й аргумент: 1 = снимки по кругу (непрерывный режим отдаёт
	 * только шум — сырой тракт валиден лишь в SNAPSHOT) */
	int snapmode = argc > 5 ? atoi(argv[5]) : 0;
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);

	int lockfd = open("/run/.camsnap.lock", O_CREAT | O_RDWR, 0644);
	if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
		printf("камера занята\n");
		return 2;
	}

	ctrl = open("/dev/msm_camera/control0", O_RDWR);
	cfg = open("/dev/msm_camera/config0", O_RDWR);
	frm = open("/dev/msm_camera/frame0", O_RDWR);
	if (ctrl < 0 || cfg < 0 || frm < 0) {
		printf("open: %s\n", strerror(errno));
		return 1;
	}
	printf("камера открыта (control/config/frame)\n");

	int pfd = open("/dev/pmem_adsp", O_RDWR);
	if (pfd < 0) { printf("pmem: %s\n", strerror(errno)); return 1; }
	size_t fsz = (RAWLEN + 4095) & ~4095u;
	size_t blen = fsz * 2 * NBUF;          /* на кадр: Y + CbCr половины */
	uint8_t *buf = mmap(NULL, blen, PROT_READ | PROT_WRITE, MAP_SHARED,
			    pfd, 0);
	if (buf == MAP_FAILED) {
		printf("mmap %zu: %s\n", blen, strerror(errno));
		return 1;
	}
	memset(buf, 0, blen);

	/* три буфера — конвейеру есть куда писать, пока мы читаем один */
	/* в режиме снимка — ОДИН буфер, как в camsnap: с тремя кадр
	 * уходит в другой слот, и мы читаем пустоту */
	int nbuf = snapmode ? 1 : NBUF;
	for (int i = 0; i < nbuf; i++) {
		struct msm_pmem_info pi;
		memset(&pi, 0, sizeof(pi));
		pi.type = MSM_PMEM_RAW_MAINIMG;
		pi.fd = pfd;
		pi.vaddr = buf + i * fsz * 2;
		pi.offset = i * fsz * 2;
		pi.len = fsz * 2;
		pi.y_off = 0;
		pi.cbcr_off = fsz;
		pi.vfe_can_write = 1;
		if (ioctl(cfg, MSM_CAM_IOCTL_REGISTER_PMEM, &pi) < 0) {
			printf("REGISTER_PMEM[%d]: %s\n", i, strerror(errno));
			return 1;
		}
	}
	printf("%d буфера зарегистрированы\n", NBUF);

	struct sensor_cfg_data sc;
	memset(&sc, 0, sizeof(sc));
	sc.cfgtype = CFG_SET_MODE;
	sc.mode = SENSOR_PREVIEW_MODE;
	sc.rs = 0;
	ioctl(cfg, MSM_CAM_IOCTL_SENSOR_IO_CFG, &sc);
	sleep(1);

	memset(&sc, 0, sizeof(sc));
	sc.cfgtype = CFG_SET_PICT_EXP_GAIN;
	sc.cfg.exp_gain.gain = gain;
	sc.cfg.exp_gain.line = line;
	ioctl(cfg, MSM_CAM_IOCTL_SENSOR_IO_CFG, &sc);
	sleep(1);

	struct camera_enable_cmd en;
	memset(&en, 0, sizeof(en));
	strcpy(en.name, "vfe");
	ioctl(cfg, MSM_CAM_IOCTL_ENABLE_VFE, &en);

	vfe_cmd(VFE_CMD_ID_RESET, NULL, 0);
	usleep(100000);

	struct vfe_cmd_camif_config cam;
	memset(&cam, 0, sizeof(cam));
	cam.camifConfig.syncMode = VFE_CAMIF_SYNC_MODE_APS;
	cam.frame.pixelsPerLine = 0x0aac;
	cam.frame.linesPerFrame = 0x03e2;
	cam.window.firstpixel = 0;
	cam.window.lastpixel = W - 1;
	cam.window.firstline = 0;
	cam.window.lastline = H - 1;
	vfe_cmd(VFE_CMD_ID_CAMIF_CONFIG, &cam, sizeof(cam));

	struct vfe_cmd_axi_output_config ax;
	memset(&ax, 0, sizeof(ax));
	ax.burstLength = VFE_AXI_BURST_LENGTH_IS_8;
	ax.outputMode = VFE_AXI_OUTPUT_MODE_CAMIFToAXIViaOutput2;
	ax.outputDataSize = VFE_RAW_PIXEL_DATA_SIZE_10BIT;
	ax.output2.fragmentCount = 1;
	ax.output2.outputY.imageWidth = W;
	ax.output2.outputY.imageHeight = H;
	ax.output2.outputY.outRowCount = H;
	ax.output2.outputY.outRowIncrement = W;
	ax.output2.outputCbcr = ax.output2.outputY;
	struct msm_vfe_command_8k axv = { 0, sizeof(ax), &ax };
	struct msm_vfe_cfg_cmd axc = { CMD_RAW_PICT_AXI_CFG, sizeof(axv), &axv };
	if (ioctl(cfg, MSM_CAM_IOCTL_AXI_CONFIG, &axc) < 0)
		printf("AXI_CONFIG: %s\n", strerror(errno));

	/* НЕПРЕРЫВНЫЙ режим — теперь законно: буферы возвращаем */
	struct vfe_cmd_start st;
	memset(&st, 0, sizeof(st));
	st.inputSource = VFE_START_INPUT_SOURCE_CAMIF;
	st.operationMode = snapmode ? VFE_START_OPERATION_MODE_SNAPSHOT
				    : VFE_START_OPERATION_MODE_CONTINUOUS;
	st.snapshotCount = 1;
	st.pixel = VFE_BAYER_GRGRGR;
	if (vfe_cmd(VFE_CMD_ID_START, &st, sizeof(st)) == 0)
		printf("поток запущен\n");

	/* авто-экспозиция ПОТОКА: в стриминге работает CFG_SET_EXP_GAIN (18),
	 * а не CFG_SET_PICT_EXP_GAIN (19) — снимочная команда на живой
	 * поток не действует, кадр остаётся тёмным. */
	int cur_gain = gain, cur_line = line;

	static uint8_t small[(W / 4) * (H / 4)];
	/* цветное превью 324x243 RGB: R/G/B из одного 2x2-блока Байера */
	static uint8_t rgb[(W / 4) * (H / 4) * 3];
	unsigned char evbuf[4096];
	time_t t0 = time(NULL);
	int frames = 0, lost = 0, saved = 0;
	struct timespec last_rec = {0, 0};
	if (recsec > 0)
		system("mkdir -p /tmp/vid; rm -f /tmp/vid/f*.raw");

	while (snapmode && !stop_now && time(NULL) - t0 < secs) {
		/* Снимок по кругу: устройство НЕ закрываем, кадр читаем
		 * прямо из буфера (как camsnap) — очередь кадров в режиме
		 * снимка не используется. */
		if (frames > 0) {
			vfe_cmd(VFE_CMD_ID_STOP, NULL, 0);
			usleep(150000);
			vfe_cmd(VFE_CMD_ID_START, &st, sizeof(st));
		}
		usleep(700000);
		const uint8_t *src = buf + fsz;
		for (int y = 0; y < H / 4; y++)
			for (int x = 0; x < W / 4; x++)
				small[y * (W / 4) + x] = src[(y * 4) * W + x * 4];
		FILE *o = fopen("/tmp/preview.tmp", "wb");
		if (o) {
			fwrite(small, 1, sizeof(small), o);
			fclose(o);
			rename("/tmp/preview.tmp", "/tmp/preview.raw");
		}
		if (recsec > 0 && time(NULL) - t0 < recsec && saved < 200) {
			char nm[48];
			sprintf(nm, "/tmp/vid/f%03d.raw", saved++);
			FILE *v = fopen(nm, "wb");
			if (v) { fwrite(src, 1, RAWLEN, v); fclose(v); }
		}
		frames++;
	}

	while (!snapmode && !stop_now && time(NULL) - t0 < secs) {
		if (0) {
			/* перезапуск снимка без закрытия устройства */
			vfe_cmd(VFE_CMD_ID_STOP, NULL, 0);
			usleep(120000);
			vfe_cmd(VFE_CMD_ID_START, &st, sizeof(st));
		}
		struct pollfd p = { .fd = frm, .events = POLLIN };
		int pr = poll(&p, 1, 1500);
		if (pr <= 0) { lost++; if (lost > 8) break; continue; }

		struct msm_frame f;
		memset(&f, 0, sizeof(f));
		if (ioctl(frm, MSM_CAM_IOCTL_GETFRAME, &f) < 0) {
			lost++;
			if (lost > 8) break;
			usleep(50000);
			continue;
		}
		lost = 0;
		frames++;

		/* кадр лежит по f.buffer + f.cbcr_off (сырой поток идёт
		 * через CbCr-канал) — уменьшаем в 4 раза и пишем */
		const uint8_t *src = (const uint8_t *)(f.buffer + f.cbcr_off);
		for (int y = 0; y < H / 4; y++)
			for (int x = 0; x < W / 4; x++) {
				const uint8_t *q = src + (y * 4) * W + x * 4;
				uint8_t *o = rgb + (y * (W / 4) + x) * 3;
				small[y * (W / 4) + x] = q[0];
				o[0] = q[1];                        /* R */
				o[1] = (q[0] + q[W + 1]) >> 1;      /* G */
				o[2] = q[W];                        /* B */
			}
		/* пишем во временный и переименовываем: читатель никогда
		 * не увидит полукадр */
		FILE *o = fopen("/tmp/preview.tmp", "wb");
		if (o) {
			fwrite(rgb, 1, sizeof(rgb), o);
			fclose(o);
			rename("/tmp/preview.tmp", "/tmp/preview.rgb");
		}
		if (frames % 50 == 0)
			printf("кадров: %d, gain %d line %d\n", frames, cur_gain, cur_line);

		/* авто-экспозиция: подстраиваем каждые ~10 кадров */
		if (frames % 10 == 0) {
			long sum = 0;
			for (size_t i = 0; i < sizeof(small); i += 7)
				sum += small[i];
			int mean = (int)(sum / (sizeof(small) / 7 + 1));
			int target = 90;
			if (mean < target - 12 || mean > target + 12) {
				double k = (double)target / (mean > 2 ? mean : 2);
				if (k > 2.0) k = 2.0;
				if (k < 0.5) k = 0.5;
				if (cur_line < 900 && k > 1.0)
					cur_line = (int)(cur_line * k);
				else
					cur_gain = (int)(cur_gain * k);
				if (cur_line > 900) cur_line = 900;
				if (cur_line < 60) cur_line = 60;
				if (cur_gain > 400) cur_gain = 400;
				if (cur_gain < 20) cur_gain = 20;
				struct sensor_cfg_data ae;
				memset(&ae, 0, sizeof(ae));
				ae.cfgtype = CFG_SET_EXP_GAIN;
				ae.mode = SENSOR_PREVIEW_MODE;
				ae.cfg.exp_gain.gain = cur_gain;
				ae.cfg.exp_gain.line = cur_line;
				ioctl(cfg, MSM_CAM_IOCTL_SENSOR_IO_CFG, &ae);
			}
		}

		/* запись видео: полный кадр ~5 раз в секунду */
		if (recsec > 0 && time(NULL) - t0 < recsec && saved < 200) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			long dms = (now.tv_sec - last_rec.tv_sec) * 1000 +
				   (now.tv_nsec - last_rec.tv_nsec) / 1000000;
			if (dms >= 200) {
				last_rec = now;
				char nm[48];
				sprintf(nm, "/tmp/vid/f%03d.raw", saved++);
				FILE *v = fopen(nm, "wb");
				if (v) {
					fwrite(src, 1, RAWLEN, v);
					fclose(v);
				}
			}
		}

		/* ГЛАВНОЕ: вернуть буфер драйверу */
		if (ioctl(frm, MSM_CAM_IOCTL_RELEASE_FRAME_BUFFER, &f) < 0)
			printf("возврат буфера: %s\n", strerror(errno));

		/* очередь событий драйвера никто не читает и она растёт
		 * (~3 события на кадр) — вычитываем и выбрасываем */
		for (int k = 0; k < 8; k++) {
			struct pollfd pc = { .fd = cfg, .events = POLLIN };
			if (poll(&pc, 1, 0) <= 0)
				break;
			struct msm_stats_event_ctrl se;
			memset(&se, 0, sizeof(se));
			se.timeout_ms = 0;
			se.stats_event.len = sizeof(evbuf);
			se.stats_event.data = evbuf;
			if (ioctl(cfg, MSM_CAM_IOCTL_GET_STATS, &se) < 0)
				break;
		}
	}

	printf("кадров: %d\n", frames);
	vfe_cmd(VFE_CMD_ID_STOP, NULL, 0);
	usleep(300000);
	close(frm);
	close(cfg);
	close(ctrl);
	printf("поток остановлен\n");
	return 0;
}
