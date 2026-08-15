#include <ewoksys/vdevice.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <ewoksys/ipc.h>
#include <ewoksys/kernel_tic.h>
#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/i2c.h>
#include "gt911/gt911.h"

#define TP_POLL_MIN_US       8000u   /* ~125Hz while touching */
#define TP_POLL_MAX_US       50000u  /* back off to 20Hz when idle */
#define TP_POLL_HOLD_US      2000u   /* wait for the next IRQ while finger stays down */
#define TP_RELEASE_DELAY_MS  20
#define TP_I2C_FAIL_MAX      20     /* consecutive failures before reinit */
#define GT911_PROFILE_LEGACY 0
#define GT911_PROFILE_DPI    1

static bool press = false;
static	TouchCordinate_t cordinate[5];
static 	uint8_t  number_of_cordinate = 0;
static 	uint64_t last_ts = 0;
static	uint16_t touch_data[3];
static	bool     has_data = false;
static	uint32_t poll_sleep_us = TP_POLL_MIN_US;
static	uint32_t i2c_fail_count = 0;

static const GT911_Platform_t gt911_platform = {
	.bus = BCM2712_I2C_BUS_HEADER,
	.sda = -1,
	.scl = -1,
	.rst = 17,
	.irq = 4,
	.addr = 0,
};

static const GT911_Platform_t gt911_platform_dpi = {
	.bus = -1,
	.sda = 10,
	.scl = 11,
	.rst = -1,
	.irq = 27,
	.addr = 0,
};

static const GT911_Platform_t* const gt911_platform_profiles[] = {
	&gt911_platform,
	&gt911_platform_dpi,
};

static const GT911_Platform_t* gt911_platform_active =
	&gt911_platform_dpi;

static bool tp_irq_asserted(void) {
	if (gt911_platform_active->irq < 0)
		return true;
	return !bcm2712_gpio_read((uint32_t)gt911_platform_active->irq);
}

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
	GT911_Status_t ret = GT911_OK;
	uint64_t now_ms = kernel_tic_ms(0);
	bool irq_asserted = tp_irq_asserted();

	/*
	 * On the DPI panel the controller exposes IRQ on GPIO27. When the line is
	 * inactive there is no pending touch data, so skip the expensive bit-bang
	 * I2C transaction and only keep a cheap GPIO poll while idle.
	 */
	if (!irq_asserted) {
		if (press && (now_ms - last_ts) > TP_RELEASE_DELAY_MS) {
			press = false;
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
			return 0;
		}

		if (press) {
			usleep(TP_POLL_HOLD_US);
			return 0;
		}

		if (poll_sleep_us < TP_POLL_MAX_US) {
			poll_sleep_us <<= 1;
			if (poll_sleep_us > TP_POLL_MAX_US)
				poll_sleep_us = TP_POLL_MAX_US;
		}
		usleep(poll_sleep_us);
		return 0;
	}

	number_of_cordinate = 0;
	ret = GT911_ReadTouch(cordinate, &number_of_cordinate);

	if (ret != GT911_OK) {
		i2c_fail_count++;
		if (i2c_fail_count >= TP_I2C_FAIL_MAX) {
			i2c_fail_count = 0;
			GT911_Init(gt911_platform_active);
		}
	} else {
		i2c_fail_count = 0;
	}

	if (ret != GT911_OK || !number_of_cordinate) {
		if (press && (now_ms - last_ts) > TP_RELEASE_DELAY_MS) {
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
		last_ts = now_ms;
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
	const char* mnt_point = "/dev/touch0";
	int c = 0;

	while ((c = getopt(argc, argv, "t:")) != -1) {
		switch (c) {
		case 't': {
			int profile = atoi(optarg);
			if (profile != GT911_PROFILE_LEGACY &&
					profile != GT911_PROFILE_DPI) {
				return -1;
			}
			gt911_platform_active = gt911_platform_profiles[profile];
			break;
		}
		default:
			return -1;
		}
	}

	if (optind < argc)
		mnt_point = argv[optind];

	GT911_Init(gt911_platform_active);
	if (gt911_platform_active->irq >= 0) {
		bcm2712_gpio_init();
		bcm2712_gpio_config((uint32_t)gt911_platform_active->irq, GPIO_FUNC_INPUT);
		bcm2712_gpio_pull((uint32_t)gt911_platform_active->irq, GPIO_PULL_UP);
	}

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "gt911");
	dev.read = tp_read;
	dev.loop_step = tp_loop;
	dev.check_poll_events = tp_check_poll_events;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
