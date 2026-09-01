/* Проба камеры HTC HD2 (msm_camera v1 + VFE8x, ядро 3.0).
 *
 * Шаг 1: открыть control0+config0, узнать сенсор, включить VFE,
 * послать VFE_CMD_ID_RESET. Каждый шаг печатает результат — по ним
 * видно, докуда доходит тракт.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#define MSM_CAM_IOCTL_MAGIC 'm'
struct msm_camsensor_info { char name[24]; uint8_t flash_enabled; };
struct camera_enable_cmd { char name[32]; };
struct msm_vfe_cfg_cmd { int cmd_type; uint16_t length; void *value; };
struct msm_vfe_command_8k { int id; uint16_t length; void *value; };

#define MSM_CAM_IOCTL_GET_SENSOR_INFO _IOR(MSM_CAM_IOCTL_MAGIC, 1, struct msm_camsensor_info *)
#define MSM_CAM_IOCTL_CONFIG_VFE      _IOW(MSM_CAM_IOCTL_MAGIC, 5, struct msm_vfe_cfg_cmd *)
#define MSM_CAM_IOCTL_ENABLE_VFE      _IOW(MSM_CAM_IOCTL_MAGIC, 8, struct camera_enable_cmd *)
#define MSM_CAM_IOCTL_DISABLE_VFE     _IOW(MSM_CAM_IOCTL_MAGIC, 11, struct camera_enable_cmd *)

#define CMD_GENERAL       0
#define VFE_CMD_ID_RESET  1

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	int ctrl = open("/dev/msm_camera/control0", O_RDWR);
	printf("control0: %s\n", ctrl >= 0 ? "открыт" : strerror(errno));
	if (ctrl < 0) return 1;

	int cfg = open("/dev/msm_camera/config0", O_RDWR);
	printf("config0: %s\n", cfg >= 0 ? "открыт" : strerror(errno));
	if (cfg < 0) return 1;

	struct msm_camsensor_info si;
	memset(&si, 0, sizeof(si));
	if (ioctl(cfg, MSM_CAM_IOCTL_GET_SENSOR_INFO, &si) < 0)
		printf("GET_SENSOR_INFO: %s\n", strerror(errno));
	else
		printf("сенсор: '%s', вспышка: %d\n", si.name, si.flash_enabled);

	struct camera_enable_cmd en;
	memset(&en, 0, sizeof(en));
	strcpy(en.name, "vfe");
	if (ioctl(cfg, MSM_CAM_IOCTL_ENABLE_VFE, &en) < 0)
		printf("ENABLE_VFE: %s\n", strerror(errno));
	else
		printf("VFE включён\n");

	struct msm_vfe_command_8k vc = { VFE_CMD_ID_RESET, 0, NULL };
	struct msm_vfe_cfg_cmd cc = { CMD_GENERAL, sizeof(vc), &vc };
	if (ioctl(cfg, MSM_CAM_IOCTL_CONFIG_VFE, &cc) < 0)
		printf("VFE RESET: %s\n", strerror(errno));
	else
		printf("VFE RESET отправлен\n");

	sleep(1);

	/* DISABLE_VFE роняет ядро (msm_camio_disable: NULL) — не зовём */
	close(cfg);
	close(ctrl);
	printf("готово\n");
	return 0;
}
