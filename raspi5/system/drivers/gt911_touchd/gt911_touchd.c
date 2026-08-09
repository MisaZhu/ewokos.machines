#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/i2c.h>
#include <ewoksys/ipc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GT911_BUS              1
#define GT911_ADDR_5D          0x5d
#define GT911_ADDR_14          0x14
#define GT911_REG_COMMAND      0x8040
#define GT911_REG_PRODUCT_ID   0x8140
#define GT911_REG_STATUS       0x814e
#define GT911_REG_POINT1       0x8150
#define GT911_RESET_GPIO       17
#define GT911_INT_GPIO         4
#define TOUCH_MAX_POINTS       5
#define POLL_ACTIVE_US         8000
#define POLL_IDLE_US           50000
#define RELEASE_DELAY_MS       20
#define I2C_REINIT_THRESHOLD   20

typedef struct {
	uint16_t x;
	uint16_t y;
} touch_point_t;

static uint8_t gt911_addr;
static uint16_t touch_event[3];
static touch_point_t last_point;
static bool pressed;
static bool has_event;
static uint64_t last_touch_ms;
static uint32_t i2c_failures;

static bool gt911_valid_product_id(const uint8_t id[4]) {
	return (id[0] || id[1] || id[2] || id[3]) &&
			!(id[0] == 0xff && id[1] == 0xff &&
			id[2] == 0xff && id[3] == 0xff);
}

static int gt911_write_reg(uint16_t reg, const uint8_t *data, int len) {
	uint8_t buf[16];
	if (len < 0 || len > (int)sizeof(buf) - 2)
		return -1;
	buf[0] = reg >> 8;
	buf[1] = reg;
	if (len)
		memcpy(buf + 2, data, len);
	return bcm2712_i2c_write(GT911_BUS, gt911_addr, buf, len + 2) < 0 ? -1 : 0;
}

static int gt911_read_reg(uint16_t reg, uint8_t *data, int len) {
	uint8_t command[2] = {reg >> 8, reg};
	return bcm2712_i2c_write_read(GT911_BUS, gt911_addr,
			command, sizeof(command), data, len) < 0 ? -1 : 0;
}

static void gt911_reset(bool int_high) {
	bcm2712_gpio_config(GT911_RESET_GPIO, GPIO_FUNC_OUTPUT);
	bcm2712_gpio_config(GT911_INT_GPIO, GPIO_FUNC_OUTPUT);
	bcm2712_gpio_pull(GT911_INT_GPIO, GPIO_PULL_NONE);
	bcm2712_gpio_write(GT911_INT_GPIO, int_high);
	bcm2712_gpio_write(GT911_RESET_GPIO, false);
	usleep(20000);
	bcm2712_gpio_write(GT911_RESET_GPIO, true);
	usleep(60000);
	bcm2712_gpio_config(GT911_INT_GPIO, GPIO_FUNC_INPUT);
	bcm2712_gpio_pull(GT911_INT_GPIO, GPIO_PULL_UP);
	usleep(20000);
}

static int gt911_init(void) {
	static const struct { uint8_t addr; bool int_high; } probes[] = {
		{GT911_ADDR_5D, false}, {GT911_ADDR_14, true}
	};
	int ret = bcm2712_i2c_init(GT911_BUS);
	if (ret < 0) {
		slog("gt911: I2C controller initialization failed: %d\n", ret);
		return -1;
	}
	bcm2712_gpio_init();
	if (bcm2712_i2c_set_speed(GT911_BUS, 400000) < 0) {
		slog("gt911: failed to switch I2C bus %d to 400kHz\n", GT911_BUS);
		return -1;
	}

	for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		uint8_t id[4] = {0};
		gt911_reset(probes[i].int_high);
		gt911_addr = probes[i].addr;
		if (!gt911_read_reg(GT911_REG_PRODUCT_ID, id, sizeof(id)) &&
			gt911_valid_product_id(id)) {
			uint8_t command = 0;
			ret = gt911_write_reg(GT911_REG_COMMAND, &command, 1);
			if (ret)
				slog("gt911: command register write failed\n");
			i2c_failures = 0;
			return ret;
		}
	}
	slog("gt911: controller not found\n");
	return -1;
}

static int gt911_read_touch(touch_point_t *point, uint8_t *count) {
	uint8_t status;
	*count = 0;
	if (gt911_read_reg(GT911_REG_STATUS, &status, 1) < 0)
		return -1;
	if (!(status & 0x80))
		return 0;

	*count = status & 0x0f;
	if (*count > TOUCH_MAX_POINTS)
		*count = 0;
	if (*count) {
		uint8_t data[6];
		if (gt911_read_reg(GT911_REG_POINT1, data, sizeof(data)) < 0)
			return -1;
		point->x = data[0] | ((uint16_t)data[1] << 8);
		point->y = data[2] | ((uint16_t)data[3] << 8);
	}
	status = 0;
	return gt911_write_reg(GT911_REG_STATUS, &status, 1);
}

static void queue_event(vdevice_t *dev, bool down, const touch_point_t *point) {
	touch_event[0] = down;
	touch_event[1] = point->x;
	touch_event[2] = point->y;
	if (!has_event) {
		has_event = true;
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	}
}

static int touch_read(vdevice_t *dev, int fd, int from_pid, fsinfo_t *node,
		void *buf, int size, int offset, void *p) {
	(void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;
	if (!has_event)
		return VFS_ERR_RETRY;
	if (size < (int)sizeof(touch_event))
		return -1;
	memcpy(buf, touch_event, sizeof(touch_event));
	has_event = false;
	return sizeof(touch_event);
}

static uint32_t touch_poll(vdevice_t *dev, int fd, int from_pid,
		fsinfo_t *node, void *p) {
	(void)dev; (void)fd; (void)from_pid; (void)node; (void)p;
	return has_event ? VFS_EVT_RD : 0;
}

static int touch_loop(vdevice_t *dev, void *p) {
	(void)p;
	touch_point_t point;
	uint8_t count;
	int ret = gt911_read_touch(&point, &count);
	if (!ret && count) {
		i2c_failures = 0;
		last_point = point;
		last_touch_ms = kernel_tic_ms(0);
		pressed = true;
		queue_event(dev, true, &point);
	} else if (!ret) {
		i2c_failures = 0;
	}
	if ((ret < 0 || !count) && pressed &&
		kernel_tic_ms(0) - last_touch_ms > RELEASE_DELAY_MS) {
		pressed = false;
		queue_event(dev, false, &last_point);
	}
	if (ret < 0) {
		i2c_failures++;
		if (i2c_failures >= I2C_REINIT_THRESHOLD) {
			i2c_failures = 0;
			gt911_init();
		}
	}
	usleep(pressed ? POLL_ACTIVE_US : POLL_IDLE_US);
	return 0;
}

int main(int argc, char **argv) {
	const char *mount_point = argc > 1 ? argv[1] : "/dev/touch0";
	_mmio_base = mmio_map();
	if (gt911_init() < 0)
		slog("gt911: initial probe failed; background retry remains enabled\n");

	vdevice_t dev;
	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "gt911");
	dev.read = touch_read;
	dev.loop_step = touch_loop;
	dev.check_poll_events = touch_poll;
	device_run(&dev, mount_point, FS_TYPE_CHAR, 0444);
	return 0;
}
