#include <ewoksys/vdevice.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ewoksys/vfs.h>
#include <ewoksys/ipc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <arch/bcm283x/i2c.h>

#define TP_POLL_MIN_US		 8000u
#define TP_POLL_MAX_US		50000u
#define TP_RELEASE_DELAY_MS	   20
#define TP_INIT_RETRY_MS	 1000
#define TP_I2C_FAIL_MAX		   20

#define DSI_TOUCH_SDA		    0
#define DSI_TOUCH_SCL		    1

#define BIT(x)			(1u << (x))

#define PANEL_MCU_ADDR		 0x45
#define PANEL_REG_TP		 0x94
#define PANEL_REG_LCD		 0x95
#define PANEL_REG_PWM		 0x96
#define PANEL_REG_SIZE		 0x97
#define PANEL_REG_ID		 0x98
#define PANEL_REG_VERSION	 0x99
#define PANEL_PWR_DEFAULT	(uint16_t)(BIT(9) | BIT(8))

#define GOODIX_ADDR_PRIMARY	 0x5d
#define GOODIX_ADDR_FALLBACK	 0x14

#define GOODIX_REG_COMMAND	0x8040
#define GOODIX_REG_ID		0x8140
#define GOODIX_REG_STATUS	0x814e
#define GOODIX_REG_POINT1	0x8150
#define GOODIX_POINT_SIZE	      8
#define GOODIX_READY_FLAG	0x80
#define GOODIX_MAX_POINTS	      5

typedef enum {
	TP_OK = 0,
	TP_ERROR = 1,
	TP_NOT_RESPONSE = 2,
	TP_NO_DATA = 3
} tp_status_t;

typedef struct {
	uint16_t x;
	uint16_t y;
} tp_point_t;

static bool press = false;
static tp_point_t point[GOODIX_MAX_POINTS];
static uint8_t point_nr = 0;
static uint64_t last_ts = 0;
static uint16_t touch_data[3];
static bool has_data = false;
static bool tp_ready = false;
static bool panel_ready = false;
static uint32_t poll_sleep_us = TP_POLL_MIN_US;
static uint32_t i2c_fail_count = 0;
static uint64_t tp_retry_ts = 0;
static uint32_t tp_retry_count = 0;
static uint8_t touch_addr = GOODIX_ADDR_PRIMARY;
static uint16_t panel_power_state = PANEL_PWR_DEFAULT;

/* raw bit-bang primitives from arch_bcm283x i2c.c */
extern void i2c_do_start(void);
extern void i2c_do_stop(void);
extern uint32_t i2c_do_write_byte(uint8_t data);
extern uint8_t i2c_do_read_byte(int32_t ack);

static void dsi_i2c_select(void) {
	i2c_init(DSI_TOUCH_SDA, DSI_TOUCH_SCL);
	proc_usleep(2000);
}

static uint32_t dsi_i2c_write_reg8(uint8_t addr, uint8_t reg, uint8_t value) {
	uint8_t buf[2];

	buf[0] = reg;
	buf[1] = value;
	return i2c_puts_raw(addr, buf, 2);
}

static uint32_t dsi_i2c_read_reg8(uint8_t addr, uint8_t reg, uint8_t* value) {
	uint32_t test = 0;
	uint8_t addr8 = (uint8_t)(addr << 1);

	i2c_do_start();
	test |= i2c_do_write_byte(addr8);
	test |= i2c_do_write_byte(reg);
	i2c_do_start();
	test |= i2c_do_write_byte(addr8 | 0x01);
	*value = i2c_do_read_byte(0);
	i2c_do_stop();
	return test;
}

static uint32_t dsi_i2c_write_reg16(uint8_t addr, uint16_t reg,
		const uint8_t* data, uint16_t len) {
	uint32_t test = 0;
	uint8_t addr8 = (uint8_t)(addr << 1);
	uint16_t i;

	i2c_do_start();
	test |= i2c_do_write_byte(addr8);
	test |= i2c_do_write_byte((uint8_t)(reg >> 8));
	test |= i2c_do_write_byte((uint8_t)(reg & 0xff));
	for (i = 0; i < len; i++)
		test |= i2c_do_write_byte(data[i]);
	i2c_do_stop();
	return test;
}

static uint32_t dsi_i2c_read_reg16(uint8_t addr, uint16_t reg,
		uint8_t* data, uint16_t len) {
	uint32_t test = 0;
	uint8_t addr8 = (uint8_t)(addr << 1);
	uint16_t i;

	i2c_do_start();
	test |= i2c_do_write_byte(addr8);
	test |= i2c_do_write_byte((uint8_t)(reg >> 8));
	test |= i2c_do_write_byte((uint8_t)(reg & 0xff));
	i2c_do_start();
	test |= i2c_do_write_byte(addr8 | 0x01);
	for (i = 0; i < len; i++)
		data[i] = i2c_do_read_byte(i + 1 < len);
	i2c_do_stop();
	return test;
}

static int panel_write_power_state(void) {
	uint8_t high = (uint8_t)(panel_power_state >> 8);
	uint8_t low = (uint8_t)(panel_power_state & 0xff);

	if (dsi_i2c_write_reg8(PANEL_MCU_ADDR, PANEL_REG_TP, high) != 0)
		return -1;
	if (dsi_i2c_write_reg8(PANEL_MCU_ADDR, PANEL_REG_LCD, low) != 0)
		return -1;
	return 0;
}

static int panel_read_info(void) {
	uint8_t id;
	uint8_t size;
	uint8_t version;

	if (dsi_i2c_read_reg8(PANEL_MCU_ADDR, PANEL_REG_ID, &id) != 0)
		return -1;
	if (dsi_i2c_read_reg8(PANEL_MCU_ADDR, PANEL_REG_SIZE, &size) != 0)
		return -1;
	if (dsi_i2c_read_reg8(PANEL_MCU_ADDR, PANEL_REG_VERSION, &version) != 0)
		return -1;

	slog("dsi_touchd: panel_mcu id=0x%02x size=%u ver=0x%02x\n",
			id, size, version);
	return 0;
}

static void panel_touch_reset_cycle(void) {
	panel_power_state &= (uint16_t)~BIT(9);
	panel_write_power_state();
	proc_usleep(20000);

	panel_power_state |= BIT(9);
	panel_write_power_state();
	proc_usleep(60000);
}

static int panel_init(void) {
	dsi_i2c_select();

	if (panel_read_info() != 0) {
		slog("dsi_touchd: panel_mcu probe failed addr=0x%02x\n",
				PANEL_MCU_ADDR);
		return -1;
	}

	panel_power_state = PANEL_PWR_DEFAULT;
	if (panel_write_power_state() != 0) {
		slog("dsi_touchd: panel_mcu power write failed\n");
		return -1;
	}

	dsi_i2c_write_reg8(PANEL_MCU_ADDR, PANEL_REG_PWM, 0xff);
	proc_usleep(20000);
	panel_ready = true;
	return 0;
}

static tp_status_t goodix_write_reg(uint16_t reg, const uint8_t* data, uint16_t len) {
	return dsi_i2c_write_reg16(touch_addr, reg, data, len) == 0 ?
		TP_OK : TP_NOT_RESPONSE;
}

static tp_status_t goodix_read_reg(uint16_t reg, uint8_t* data, uint16_t len) {
	return dsi_i2c_read_reg16(touch_addr, reg, data, len) == 0 ?
		TP_OK : TP_NOT_RESPONSE;
}

static tp_status_t goodix_set_command(uint8_t value) {
	return goodix_write_reg(GOODIX_REG_COMMAND, &value, 1);
}

static tp_status_t goodix_probe_addr(uint8_t addr, uint8_t* id_buf) {
	touch_addr = addr;
	return goodix_read_reg(GOODIX_REG_ID, id_buf, 4);
}

static tp_status_t goodix_init(void) {
	static const uint8_t candidates[] = {
		GOODIX_ADDR_PRIMARY,
		GOODIX_ADDR_FALLBACK,
	};
	uint8_t id_buf[4];
	uint32_t i;

	if (!panel_ready && panel_init() != 0)
		return TP_NOT_RESPONSE;

	for (i = 0; i < sizeof(candidates); i++) {
		memset(id_buf, 0, sizeof(id_buf));
		if (goodix_probe_addr(candidates[i], id_buf) != TP_OK)
			continue;
		if ((id_buf[0] == 0 && id_buf[1] == 0 &&
					id_buf[2] == 0 && id_buf[3] == 0) ||
				(id_buf[0] == 0xff && id_buf[1] == 0xff &&
				 id_buf[2] == 0xff && id_buf[3] == 0xff))
			continue;

		slog("dsi_touchd: goodix ready addr=0x%02x id=%c%c%c%c\n",
				touch_addr, id_buf[0], id_buf[1], id_buf[2], id_buf[3]);
		goodix_set_command(0x00);
		return TP_OK;
	}

	panel_touch_reset_cycle();

	memset(id_buf, 0, sizeof(id_buf));
	if (goodix_probe_addr(GOODIX_ADDR_PRIMARY, id_buf) == TP_OK &&
			!(id_buf[0] == 0 && id_buf[1] == 0 &&
			  id_buf[2] == 0 && id_buf[3] == 0) &&
			!(id_buf[0] == 0xff && id_buf[1] == 0xff &&
			  id_buf[2] == 0xff && id_buf[3] == 0xff)) {
		slog("dsi_touchd: goodix ready addr=0x%02x id=%c%c%c%c after reset\n",
				touch_addr, id_buf[0], id_buf[1], id_buf[2], id_buf[3]);
		goodix_set_command(0x00);
		return TP_OK;
	}

	slog("dsi_touchd: goodix probe failed sda=%d scl=%d\n",
			DSI_TOUCH_SDA, DSI_TOUCH_SCL);
	return TP_NOT_RESPONSE;
}

static tp_status_t goodix_read_touch(tp_point_t* pts, uint8_t* nr) {
	uint8_t status;
	uint8_t buf[GOODIX_POINT_SIZE];
	uint8_t i;

	if (goodix_read_reg(GOODIX_REG_STATUS, &status, 1) != TP_OK)
		return TP_NOT_RESPONSE;
	if ((status & GOODIX_READY_FLAG) == 0)
		return TP_NO_DATA;

	*nr = status & 0x0f;
	if (*nr > GOODIX_MAX_POINTS)
		*nr = GOODIX_MAX_POINTS;

	for (i = 0; i < *nr; i++) {
		if (goodix_read_reg((uint16_t)(GOODIX_REG_POINT1 + i * GOODIX_POINT_SIZE),
					buf, sizeof(buf)) != TP_OK)
			return TP_NOT_RESPONSE;
		pts[i].x = (uint16_t)((buf[1] << 8) | buf[0]);
		pts[i].y = (uint16_t)((buf[3] << 8) | buf[2]);
	}

	status = 0;
	goodix_write_reg(GOODIX_REG_STATUS, &status, 1);
	return TP_OK;
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
	tp_status_t ret;
	bool need_wakeup = false;
	uint64_t now_ms = kernel_tic_ms(0);

	(void)p;

	if (!tp_ready) {
		if (now_ms >= tp_retry_ts) {
			tp_retry_count++;
			slog("dsi_touchd: init_try count=%u\n", tp_retry_count);
			if (goodix_init() == TP_OK) {
				tp_ready = true;
				i2c_fail_count = 0;
				poll_sleep_us = TP_POLL_MIN_US;
			}
			else {
				tp_retry_ts = now_ms + TP_INIT_RETRY_MS;
			}
		}

		usleep(TP_POLL_MAX_US);
		return 0;
	}

	point_nr = 0;
	ret = goodix_read_touch(point, &point_nr);

	if (ret == TP_NOT_RESPONSE) {
		i2c_fail_count++;
		if (i2c_fail_count >= TP_I2C_FAIL_MAX) {
			i2c_fail_count = 0;
			tp_ready = false;
			tp_retry_ts = now_ms;
			panel_ready = false;
			slog("dsi_touchd: link_lost addr=0x%02x\n", touch_addr);
		}
	}
	else if (ret == TP_OK || ret == TP_NO_DATA) {
		i2c_fail_count = 0;
	}

	if (ret == TP_OK && !point_nr) {
		if (press && (now_ms - last_ts) > TP_RELEASE_DELAY_MS) {
			press = false;
			touch_data[0] = press;
			touch_data[1] = point[0].x;
			touch_data[2] = point[0].y;
			if (!has_data) {
				has_data = true;
				need_wakeup = true;
			}
		}
	}
	else if (ret == TP_OK && point_nr) {
		last_ts = now_ms;
		press = true;
		touch_data[0] = press;
		touch_data[1] = point[0].x;
		touch_data[2] = point[0].y;
		if (!has_data) {
			has_data = true;
			need_wakeup = true;
		}
	}

	if (need_wakeup) {
		poll_sleep_us = TP_POLL_MIN_US;
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	}
	else if (poll_sleep_us < TP_POLL_MAX_US) {
		poll_sleep_us <<= 1;
		if (poll_sleep_us > TP_POLL_MAX_US)
			poll_sleep_us = TP_POLL_MAX_US;
	}

	usleep(poll_sleep_us);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = "/dev/touch0";
	vdevice_t dev;

	if (argc > 1)
		mnt_point = argv[1];

	_mmio_base = mmio_map();

	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "dsi_touch");
	dev.read = tp_read;
	dev.loop_step = tp_loop;
	dev.check_poll_events = tp_check_poll_events;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
