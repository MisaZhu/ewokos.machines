#include <ewoksys/vdevice.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <ewoksys/ipc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/mmio.h>
#include "gt911/gt911.h"

#define TP_POLL_MIN_US       8000u   /* ~125Hz while touching */
#define TP_POLL_MAX_US       50000u  /* back off to 20Hz when idle */
#define TP_RELEASE_DELAY_MS  20
#define TP_I2C_FAIL_MAX      20     /* consecutive failures before reinit */

static bool press = false;
static	TouchCordinate_t cordinate[5];
static 	uint8_t  number_of_cordinate = 0;
static 	uint64_t last_ts = 0;
static	uint16_t touch_data[3];
static	bool     has_data = false;
static	uint32_t poll_sleep_us = TP_POLL_MIN_US;
static	uint32_t i2c_fail_count = 0;

static int tp_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)offset;
	(void)p;

	if (!has_data)
		return VFS_ERR_RETRY;
	if (size < 6)
		return -1;

	memcpy(buf, touch_data, 6);
	has_data = false;
	return 6;
}

static uint32_t tp_check_poll_events(vdevice_t* dev, int fd, int from_pid,
		fsinfo_t* node, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)p;

	return has_data ? VFS_EVT_RD : 0;
}

static int tp_loop(vdevice_t* dev, void* p) {
	(void)p;
	bool need_wakeup = false;

	number_of_cordinate = 0;
	GT911_Status_t ret = GT911_ReadTouch(cordinate, &number_of_cordinate);

	if (ret != GT911_OK) {
		i2c_fail_count++;
		if (i2c_fail_count >= TP_I2C_FAIL_MAX) {
			i2c_fail_count = 0;
			GT911_Init();
		}
	} else {
		i2c_fail_count = 0;
	}

	if (ret != GT911_OK || !number_of_cordinate) {
		if (press && (kernel_tic_ms(0) - last_ts) > TP_RELEASE_DELAY_MS) {
			press = false;
			touch_data[0] = press;
			touch_data[1] = cordinate[0].x;
			touch_data[2] = cordinate[0].y;
			if (!has_data) {
				has_data = true;
				need_wakeup = true;
			}
		}
	} else {
		last_ts = kernel_tic_ms(0);
		press = true;
		touch_data[0] = press;
		touch_data[1] = cordinate[0].x;
		touch_data[2] = cordinate[0].y;
		if (!has_data) {
			has_data = true;
			need_wakeup = true;
		}
	}

	if (need_wakeup) {
		poll_sleep_us = TP_POLL_MIN_US;
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	} else if (poll_sleep_us < TP_POLL_MAX_US) {
		poll_sleep_us <<= 1;
		if (poll_sleep_us > TP_POLL_MAX_US)
			poll_sleep_us = TP_POLL_MAX_US;
	}

	usleep(poll_sleep_us);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1]: "/dev/touch0";
	_mmio_base = mmio_map();

    GT911_Init();

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "gt911");
	dev.read = tp_read;
	dev.loop_step = tp_loop;
	dev.check_poll_events = tp_check_poll_events;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
