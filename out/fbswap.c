/* Меняет местами красный и синий каналы кадрового буфера msmfb.
 * Панель HD2 физически BGR, а драйвер объявляет RGB-маски - в X красный
 * и синий переставлены. Ставим red.offset<->blue.offset. Штатный ioctl,
 * при перезагрузке сбрасывается. */
#include <stdio.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>

int main(void) {
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	struct fb_var_screeninfo v;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0) { perror("GET"); return 1; }
	printf("было: R@%u G@%u B@%u\n", v.red.offset, v.green.offset, v.blue.offset);
	__u32 r = v.red.offset;
	v.red.offset = v.blue.offset;
	v.blue.offset = r;
	v.activate = FB_ACTIVATE_NOW;
	if (ioctl(fd, FBIOPUT_VSCREENINFO, &v) < 0) { perror("PUT"); return 1; }
	ioctl(fd, FBIOGET_VSCREENINFO, &v);
	printf("стало: R@%u G@%u B@%u\n", v.red.offset, v.green.offset, v.blue.offset);
	return 0;
}
