/* camshot — снимок камерой HTC HD2: захват и проявка.
 *
 * Захват делает camsnap (наш RAW-стек s5k3e2fx+VFE8x), здесь — вся
 * арифметика: дебайер GRBG, баланс белого «серый мир», тональная
 * кривая, лёгкая резкость и фильтры. Готовый кадр сохраняет ffmpeg
 * (libjpeg в системе нет).
 *
 *     camshot <режим> <вспышка> <фильтр>
 *         режим:   Авто | Ночь | HDR
 *         вспышка: Авто | Вкл | Выкл
 *         фильтр:  «Без фильтра» | Ч/Б | Сепия | Яркий | Мягкий
 *     camshot --raw <файл> [фильтр]
 *         проявка готового Bayer-кадра, без камеры — этим проверяется
 *         вся арифметика, когда снимать нельзя
 *
 * Ход работы печатается построчно, последняя строка — «СОХРАНЕНО <файл>».
 *
 * ВНИМАНИЕ: сам полнокадровый захват (camsnap) на этом телефоне вешает
 * ядро — см. docs/status.md. Здесь это не лечится; арифметика проверена
 * отдельно, на кадрах из файла.
 *
 * Выравнивание кадров в ночном режиме сделано перебором сдвигов по
 * уменьшенной копии, а не фазовой корреляцией: своего БПФ ради сдвига в
 * несколько точек писать незачем, а перебор ещё и устойчивее к шуму.
 *
 * Сборка: gcc -O2 camshot.c -lm -o camshot
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdarg.h>
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
#define OUT "/root/Pictures"
#define TORCH "/sys/class/leds/flashlight/brightness"

static unsigned char raw[W * H];
static float acc_r[OW * OH], acc_g[OW * OH], acc_b[OW * OH];
static float img_r[OW * OH], img_g[OW * OH], img_b[OW * OH];
static unsigned char rgb8[OW * OH * 3];

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static void torch(int on)
{
	int fd = open(TORCH, O_WRONLY | O_TRUNC);
	if (fd < 0)
		return;
	if (write(fd, on ? "255" : "0", on ? 3 : 1) < 0) { }
	close(fd);
}

/* один кадр в raw[]; 0 — не вышло */
static int capture(int gain, int line)
{
	char cmd[160];
	snprintf(cmd, sizeof(cmd),
		 "timeout 60 /usr/local/bin/camsnap %d %d >/dev/null 2>&1",
		 gain, line);
	if (system(cmd)) { }
	int fd = open("/tmp/raw1.bin", O_RDONLY);
	if (fd < 0)
		return 0;
	size_t got = 0;
	ssize_t n;
	while (got < sizeof(raw) && (n = read(fd, raw + got, sizeof(raw) - got)) > 0)
		got += (size_t)n;
	close(fd);
	return got == sizeof(raw);
}

static int load_raw(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	size_t got = 0;
	ssize_t n;
	while (got < sizeof(raw) && (n = read(fd, raw + got, sizeof(raw) - got)) > 0)
		got += (size_t)n;
	close(fd);
	return got == sizeof(raw);
}

static double mean_raw(void)
{
	double s = 0;
	for (size_t i = 0; i < sizeof(raw); i++)
		s += raw[i];
	return s / sizeof(raw);
}

/* уровень чёрного: заданный процентиль по гистограмме кадра */
static int black_level(double frac)
{
	long hist[256];
	memset(hist, 0, sizeof(hist));
	for (size_t i = 0; i < sizeof(raw); i++)
		hist[raw[i]]++;
	long need = (long)(sizeof(raw) * frac), acc = 0;
	for (int v = 0; v < 256; v++) {
		acc += hist[v];
		if (acc >= need)
			return v;
	}
	return 0;
}

/* GRBG -> RGB половинного размера, баланс белого «серый мир».
 * Растяжки яркости тут НЕТ: тянуть один раз — работа проявки, иначе
 * снимок выбеливает. */
static void debayer(float *R, float *G, float *B)
{
	int black = black_level(0.005);
	double sr = 0, sg = 0, sb = 0;
	for (int y = 0; y < OH; y++)
		for (int x = 0; x < OW; x++) {
			int g1 = raw[(2 * y) * W + 2 * x] - black;
			int r  = raw[(2 * y) * W + 2 * x + 1] - black;
			int b  = raw[(2 * y + 1) * W + 2 * x] - black;
			int g2 = raw[(2 * y + 1) * W + 2 * x + 1] - black;
			if (g1 < 0) g1 = 0;
			if (r < 0) r = 0;
			if (b < 0) b = 0;
			if (g2 < 0) g2 = 0;
			float g = (g1 + g2) / 2.0f;
			int i = y * OW + x;
			R[i] = r;
			G[i] = g;
			B[i] = b;
			sr += r; sg += g; sb += b;
		}
	double n = (double)OW * OH;
	double m = (sr + sg + sb) / (3 * n);
	double kr = m / (sr / n > 1 ? sr / n : 1);
	double kb = m / (sb / n > 1 ? sb / n : 1);
	for (int i = 0; i < OW * OH; i++) {
		R[i] = (float)(R[i] * kr / 255.0);
		G[i] = (float)(G[i] / 255.0);
		B[i] = (float)(B[i] * kb / 255.0);
	}
}

/* сдвиг кадра относительно опорного перебором по уменьшенной копии */
static void align_to(const float *base, float *o_r, float *o_g, float *o_b)
{
	const int STEP = 4, RANGE = 8;
	int bw = OW / STEP, bh = OH / STEP;
	int best_dx = 0, best_dy = 0;
	double best = 1e30;
	for (int dy = -RANGE; dy <= RANGE; dy++)
		for (int dx = -RANGE; dx <= RANGE; dx++) {
			double sad = 0;
			for (int y = RANGE; y < bh - RANGE; y++)
				for (int x = RANGE; x < bw - RANGE; x++) {
					int bi = (y * STEP) * OW + x * STEP;
					int oi = (y * STEP + dy * STEP) * OW +
						 (x * STEP + dx * STEP);
					if (oi < 0 || oi >= OW * OH)
						continue;
					double d = base[bi] - o_g[oi];
					sad += d < 0 ? -d : d;
				}
			if (sad < best) {
				best = sad;
				best_dx = dx;
				best_dy = dy;
			}
		}
	if (!best_dx && !best_dy)
		return;
	int sx = best_dx * STEP, sy = best_dy * STEP;
	static float tr[OW * OH], tg[OW * OH], tb[OW * OH];
	for (int y = 0; y < OH; y++)
		for (int x = 0; x < OW; x++) {
			int oy = y + sy, ox = x + sx;
			if (oy < 0) oy = 0;
			if (oy >= OH) oy = OH - 1;
			if (ox < 0) ox = 0;
			if (ox >= OW) ox = OW - 1;
			int i = y * OW + x, o = oy * OW + ox;
			tr[i] = o_r[o];
			tg[i] = o_g[o];
			tb[i] = o_b[o];
		}
	memcpy(o_r, tr, sizeof(tr));
	memcpy(o_g, tg, sizeof(tg));
	memcpy(o_b, tb, sizeof(tb));
}

/* Тональная кривая: света под потолок, середина к серой карте.
 * Раньше яркость растягивалась дважды (тут и в дебайере) — снимки
 * выходили выбеленными. */
static void finish(const char *filt)
{
	int n = OW * OH;
	/* 99.5-й процентиль по всем каналам */
	long hist[1024];
	memset(hist, 0, sizeof(hist));
	float top = 0;
	for (int i = 0; i < n; i++) {
		if (img_r[i] > top) top = img_r[i];
		if (img_g[i] > top) top = img_g[i];
		if (img_b[i] > top) top = img_b[i];
	}
	if (top <= 0)
		top = 1;
	for (int i = 0; i < n; i++) {
		hist[(int)(img_r[i] / top * 1023)]++;
		hist[(int)(img_g[i] / top * 1023)]++;
		hist[(int)(img_b[i] / top * 1023)]++;
	}
	long need = (long)(3.0 * n * 0.995), acc = 0;
	double hi = top;
	for (int v = 0; v < 1024; v++) {
		acc += hist[v];
		if (acc >= need) {
			hi = top * (v + 1) / 1024.0;
			break;
		}
	}
	if (hi < 1e-4)
		hi = 1e-4;

	double sum = 0;
	for (int i = 0; i < n; i++) {
		double r = img_r[i] / hi, g = img_g[i] / hi, b = img_b[i] / hi;
		if (r > 1) r = 1;
		if (g > 1) g = 1;
		if (b > 1) b = 1;
		img_r[i] = r; img_g[i] = g; img_b[i] = b;
		sum += r + g + b;
	}
	double mean = sum / (3.0 * n);
	if (mean < 1e-4)
		mean = 1e-4;
	double k = 0.20 / mean;             /* среднюю яркость к ~0,20 */
	if (k < 0.35) k = 0.35;
	if (k > 3.0) k = 3.0;

	for (int i = 0; i < n; i++) {
		double v[3] = {img_r[i] * k, img_g[i] * k, img_b[i] * k};
		for (int c = 0; c < 3; c++) {
			if (v[c] > 1) v[c] = 1;
			v[c] = pow(v[c], 1 / 2.2);
			/* мягкая S-кривая: тени глубже, света мягче */
			v[c] = (v[c] - 0.5) * 1.25 + 0.5;
			if (v[c] < 0) v[c] = 0;
			if (v[c] > 1) v[c] = 1;
		}
		double r = v[0], g = v[1], b = v[2];

		if (!strcmp(filt, "Ч/Б")) {
			double y = 0.299 * r + 0.587 * g + 0.114 * b;
			r = g = b = y;
		} else if (!strcmp(filt, "Сепия")) {
			double y = 0.299 * r + 0.587 * g + 0.114 * b;
			r = y * 1.07; g = y * 0.74; b = y * 0.43;
		} else if (!strcmp(filt, "Яркий")) {
			double y = 0.299 * r + 0.587 * g + 0.114 * b;
			r = y + (r - y) * 1.5;      /* насыщенность */
			g = y + (g - y) * 1.5;
			b = y + (b - y) * 1.5;
			r = (r - 0.5) * 1.15 + 0.5; /* контраст */
			g = (g - 0.5) * 1.15 + 0.5;
			b = (b - 0.5) * 1.15 + 0.5;
		}
		if (r < 0) r = 0;
		if (r > 1) r = 1;
		if (g < 0) g = 0;
		if (g > 1) g = 1;
		if (b < 0) b = 0;
		if (b > 1) b = 1;
		rgb8[i * 3]     = (unsigned char)(r * 255);
		rgb8[i * 3 + 1] = (unsigned char)(g * 255);
		rgb8[i * 3 + 2] = (unsigned char)(b * 255);
	}

	if (!strcmp(filt, "Мягкий")) {
		/* размытие 3x3 и подмешивание — как Gaussian blur + blend */
		static unsigned char tmp[OW * OH * 3];
		memcpy(tmp, rgb8, sizeof(tmp));
		for (int y = 1; y < OH - 1; y++)
			for (int x = 1; x < OW - 1; x++)
				for (int c = 0; c < 3; c++) {
					int s = 0;
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++)
							s += tmp[((y + dy) * OW + x + dx) * 3 + c];
					int blur = s / 9;
					int i = (y * OW + x) * 3 + c;
					rgb8[i] = (unsigned char)(tmp[i] * 0.55 +
								  blur * 0.45);
				}
	} else {
		/* лёгкая резкость: исходник плюс разница с размытием */
		static unsigned char tmp[OW * OH * 3];
		memcpy(tmp, rgb8, sizeof(tmp));
		for (int y = 1; y < OH - 1; y++)
			for (int x = 1; x < OW - 1; x++)
				for (int c = 0; c < 3; c++) {
					int s = 0;
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++)
							s += tmp[((y + dy) * OW + x + dx) * 3 + c];
					int blur = s / 9;
					int i = (y * OW + x) * 3 + c;
					int v = tmp[i] + (tmp[i] - blur) * 0.6;
					if (v < 0) v = 0;
					if (v > 255) v = 255;
					rgb8[i] = (unsigned char)v;
				}
	}
}

/* угол поворота матрицы относительно портретного экрана */
static int rotation(void)
{
	int fd = open("/root/.camrot", O_RDONLY);
	if (fd < 0)
		return 90;
	char b[16] = {0};
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	int v = n > 0 ? atoi(b) : 90;
	return (v == 0 || v == 90 || v == 180 || v == 270) ? v : 90;
}

static int save(const char *filt)
{
	(void)filt;
	int rot = rotation();
	char name[128];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(name, sizeof(name), OUT "/photo-%Y%m%d-%H%M%S.jpg", &tm);
	mkdir(OUT, 0755);

	/* поворот отдаём ffmpeg: transpose=1 — по часовой, 2 — против */
	const char *vf = "";
	if (rot == 90) vf = "-vf transpose=1";
	else if (rot == 270) vf = "-vf transpose=2";
	else if (rot == 180) vf = "-vf transpose=1,transpose=1";

	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "ffmpeg -v quiet -y -f rawvideo -pix_fmt rgb24 -s %dx%d -i - "
		 "%s -q:v 3 -frames:v 1 \"%s\"", OW, OH, vf, name);
	FILE *f = popen(cmd, "w");
	if (!f) {
		say("ОШИБКА не запустился ffmpeg");
		return 1;
	}
	fwrite(rgb8, 1, sizeof(rgb8), f);
	pclose(f);

	/* уменьшенная копия для окна камеры */
	snprintf(cmd, sizeof(cmd),
		 "ffmpeg -v quiet -y -i \"%s\" -vf scale=464:-1 -frames:v 1 "
		 "/tmp/camshot-preview.ppm", name);
	if (system(cmd)) { }

	struct stat st;
	if (stat(name, &st) != 0 || st.st_size == 0) {
		say("ОШИБКА снимок не сохранился");
		return 1;
	}
	say("СОХРАНЕНО %s", name);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "Авто";
	const char *flash = argc > 2 ? argv[2] : "Авто";
	const char *filt = argc > 3 ? argv[3] : "Без фильтра";

	if (argc > 2 && !strcmp(argv[1], "--raw")) {
		/* проявка готового кадра: этим проверяется арифметика */
		filt = argc > 3 ? argv[3] : "Без фильтра";
		if (!load_raw(argv[2])) {
			say("ОШИБКА не прочитался кадр %s", argv[2]);
			return 1;
		}
		say("проявка кадра из файла…");
		debayer(img_r, img_g, img_b);
		finish(filt);
		return save(filt);
	}

	if (!strcmp(mode, "Ночь")) {
		for (int i = 0; i < 3; i++) {
			say("ночной режим: кадр %d/3…", i + 1);
			if (!capture(180, 2800)) {
				say("ОШИБКА кадр не получен");
				return 1;
			}
			debayer(img_r, img_g, img_b);
			if (i == 0) {
				memcpy(acc_r, img_r, sizeof(acc_r));
				memcpy(acc_g, img_g, sizeof(acc_g));
				memcpy(acc_b, img_b, sizeof(acc_b));
			} else {
				align_to(acc_g, img_r, img_g, img_b);
				for (int k = 0; k < OW * OH; k++) {
					acc_r[k] += img_r[k];
					acc_g[k] += img_g[k];
					acc_b[k] += img_b[k];
				}
			}
		}
		say("складываю кадры…");
		for (int k = 0; k < OW * OH; k++) {
			img_r[k] = acc_r[k] / 3;
			img_g[k] = acc_g[k] / 3;
			img_b[k] = acc_b[k] / 3;
		}
	} else if (!strcmp(mode, "HDR")) {
		say("HDR: тёмный кадр…");
		if (!capture(60, 350)) {
			say("ОШИБКА кадр не получен");
			return 1;
		}
		debayer(acc_r, acc_g, acc_b);          /* тёмный — в acc */
		say("HDR: светлый кадр…");
		if (!capture(150, 2400)) {
			say("ОШИБКА кадр не получен");
			return 1;
		}
		debayer(img_r, img_g, img_b);
		for (int k = 0; k < OW * OH; k++) {
			double lum = (img_r[k] + img_g[k] + img_b[k]) / 3;
			double w = (lum - 0.55) * 6;       /* пересветы — из тёмного */
			if (w < 0) w = 0;
			if (w > 1) w = 1;
			img_r[k] = (float)(img_r[k] * (1 - w) + acc_r[k] * w);
			img_g[k] = (float)(img_g[k] * (1 - w) + acc_g[k] * w);
			img_b[k] = (float)(img_b[k] * (1 - w) + acc_b[k] * w);
		}
	} else {
		say("замер экспозиции…");
		if (!capture(60, 400)) {
			say("ОШИБКА кадр не получен");
			return 1;
		}
		double m = mean_raw();
		double f = 105.0 / (m > 2 ? m : 2);
		double line = 400 * f;
		if (line < 100) line = 100;
		if (line > 2800) line = 2800;
		double gain = 60;
		if (line >= 2800 && f > 7) {
			gain = 60 * f * 400 / 2800;
			if (gain > 250)
				gain = 250;
		}
		int dark = (m < 35) || (line >= 2800 && f > 3);
		int use_flash = !strcmp(flash, "Вкл") ||
				(!strcmp(flash, "Авто") && dark);
		if (use_flash) {
			say("темно (замер %d) — вспышка", (int)m);
			torch(1);
			usleep(400000);     /* диоду нужно разгореться */
		}
		say("снимок (усиление %d, выдержка %d)…", (int)gain, (int)line);
		int ok = capture((int)gain, (int)line);
		torch(0);
		if (!ok) {
			say("ОШИБКА кадр не получен");
			return 1;
		}
		debayer(img_r, img_g, img_b);
	}

	say("проявка…");
	finish(filt);
	return save(filt);
}
