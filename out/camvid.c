/* camvid — сборка видео HTC HD2 из снятых кадров.
 *
 * Кадры уже сняты живым потоком (campreview … <секунд_записи>) и лежат
 * в /tmp/vid/f*.raw — это сырой Bayer 1296x972 по байту на точку.
 * Здесь только проявка и сборка в MP4 через ffmpeg.
 *
 * Дебайер простой, 2x2 (каждый квартет GRBG даёт одну точку), поэтому
 * итог вдвое меньше — 648x486. Баланс белого и уровень белого берутся
 * ОДИН раз по первому кадру и дальше не меняются: если считать их на
 * каждом кадре, видео мерцает.
 *
 * camvid [секунд]   (по умолчанию 10) -> /root/Pictures/video-*.mp4
 *
 * Сборка: gcc -O2 camvid.c -lm -o camvid
 */
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define W  1296
#define H  972
#define OW (W / 2)
#define OH (H / 2)
#define MAXF 512

static unsigned char raw[W * H];
static unsigned char rgb[OW * OH * 3];

static int cmp_name(const void *a, const void *b)
{
	return strcmp(*(const char **)a, *(const char **)b);
}

/* прочитать кадр целиком; 0 — не вышло */
static int read_raw(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	size_t got = 0;
	ssize_t n;
	while (got < sizeof(raw) &&
	       (n = read(fd, raw + got, sizeof(raw) - got)) > 0)
		got += (size_t)n;
	close(fd);
	return got == sizeof(raw);
}

int main(int argc, char **argv)
{
	int sec = argc > 1 ? atoi(argv[1]) : 10;
	if (sec < 1)
		sec = 1;

	/* собрать и упорядочить список кадров */
	DIR *d = opendir("/tmp/vid");
	if (!d) {
		printf("нет кадров для сборки\n");
		return 1;
	}
	char *names[MAXF];
	int nf = 0;
	struct dirent *e;
	while ((e = readdir(d)) && nf < MAXF) {
		if (e->d_name[0] != 'f')
			continue;
		size_t l = strlen(e->d_name);
		if (l < 5 || strcmp(e->d_name + l - 4, ".raw"))
			continue;
		char *p = malloc(l + 16);
		if (!p)
			break;
		sprintf(p, "/tmp/vid/%s", e->d_name);
		names[nf++] = p;
	}
	closedir(d);
	if (!nf) {
		printf("нет кадров для сборки\n");
		return 1;
	}
	qsort(names, nf, sizeof(char *), cmp_name);
	printf("кадров: %d, собираю MP4…\n", nf);

	int fps = (int)(nf / (double)sec + 0.5);
	if (fps < 1)
		fps = 1;
	char name[128];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(name, sizeof(name), "/root/Pictures/video-%Y%m%d-%H%M%S.mp4",
		 &tm);
	mkdir("/root/Pictures", 0755);

	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "ffmpeg -v quiet -y -f rawvideo -pix_fmt rgb24 -s %dx%d "
		 "-r %d -i - -c:v mpeg4 -q:v 6 \"%s\"", OW, OH, fps, name);
	FILE *ff = popen(cmd, "w");
	if (!ff) {
		printf("не запустился ffmpeg\n");
		return 1;
	}

	double kr = 1, kb = 1, white = 255;
	int have_wb = 0;
	for (int i = 0; i < nf; i++) {
		if (!read_raw(names[i]))
			continue;
		if (!have_wb) {
			/* «серый мир» и уровень белого по первому кадру */
			double sr = 0, sg = 0, sb = 0;
			long hist[256];
			memset(hist, 0, sizeof(hist));
			for (int y = 0; y < OH; y++)
				for (int x = 0; x < OW; x++) {
					int g1 = raw[(2 * y) * W + 2 * x];
					int r = raw[(2 * y) * W + 2 * x + 1];
					int b = raw[(2 * y + 1) * W + 2 * x];
					int g2 = raw[(2 * y + 1) * W + 2 * x + 1];
					int g = (g1 + g2) / 2;
					sr += r; sg += g; sb += b;
					hist[g]++;
				}
			double n = (double)OW * OH;
			double m = (sr + sg + sb) / (3 * n);
			kr = m / (sr / n > 1 ? sr / n : 1);
			kb = m / (sb / n > 1 ? sb / n : 1);
			long acc = 0, need = (long)(n * 0.99);
			white = 255;
			for (int v = 0; v < 256; v++) {   /* 99-й процентиль */
				acc += hist[v];
				if (acc >= need) { white = v; break; }
			}
			if (white < 1)
				white = 1;
			have_wb = 1;
		}
		for (int y = 0; y < OH; y++)
			for (int x = 0; x < OW; x++) {
				int g1 = raw[(2 * y) * W + 2 * x];
				int r = raw[(2 * y) * W + 2 * x + 1];
				int b = raw[(2 * y + 1) * W + 2 * x];
				int g2 = raw[(2 * y + 1) * W + 2 * x + 1];
				double vg = (g1 + g2) / 2.0 / white;
				double vr = r * kr / white;
				double vb = b * kb / white;
				if (vr > 1) vr = 1;
				if (vg > 1) vg = 1;
				if (vb > 1) vb = 1;
				unsigned char *o = rgb + (y * OW + x) * 3;
				o[0] = (unsigned char)(pow(vr, 1 / 2.2) * 255);
				o[1] = (unsigned char)(pow(vg, 1 / 2.2) * 255);
				o[2] = (unsigned char)(pow(vb, 1 / 2.2) * 255);
			}
		if (fwrite(rgb, 1, sizeof(rgb), ff) != sizeof(rgb))
			break;
	}
	pclose(ff);

	for (int i = 0; i < nf; i++) {
		unlink(names[i]);
		free(names[i]);
	}
	struct stat st;
	if (stat(name, &st) == 0)
		printf("готово: %s %lld байт\n", name, (long long)st.st_size);
	else
		printf("файл не создался: %s\n", name);
	return 0;
}
