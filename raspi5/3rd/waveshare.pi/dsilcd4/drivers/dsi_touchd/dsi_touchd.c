#include <ewoksys/vdevice.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ewoksys/vfs.h>
#include <ewoksys/ipc.h>
#include <ewoksys/kernel_tic.h>
#include <arch/bcm2712/i2c.h>
#include <arch/bcm2712/gpio.h>

/*
 * Touch daemon for DSI LCD panels on Raspberry Pi 5 (BCM2712).
 *
 * Port of the raspix dsi_touchd: same controllers (Goodix GT9xx and
 * FocalTech FT5x06, optionally powered through the Waveshare panel
 * MCU at 0x45), but all I2C goes through the RP1 DW_apb_i2c masters
 * instead of the BCM283x bit-bang/BSC paths:
 *
 *   i2c4 GPIO40/41  DSI FPC ribbon (i2c_csi_dsi), the Waveshare MCU
 *                   and its touch controller live here by default
 *   i2c1 GPIO2/3    40-pin header / DIP switch "I2C1" position
 */

#define TP_POLL_MIN_US           8000u
#define TP_POLL_MAX_US          50000u
#define TP_RELEASE_DELAY_MS        20
#define TP_RELEASE_TIMEOUT_MS      80
#define TP_INIT_RETRY_MS         1000
#define TP_I2C_FAIL_MAX            20
#define TP_I2C_RETRY_MAX            3
#define TP_I2C_SPLIT_DELAY_US      100u
#define TP_TOUCH_CACHE_SIZE        32u

#define GOODIX_ADDR_PRIMARY      0x14
#define GOODIX_ADDR_FALLBACK     0x5d
#define FT5X06_ADDR              0x38
#define DISPLAY_MCU_ADDR         0x45

#define GOODIX_REG_COMMAND      0x8040
#define GOODIX_REG_ID           0x8140
#define GOODIX_REG_STATUS       0x814e
#define GOODIX_REG_POINT1       0x8150
#define GOODIX_POINT_SIZE             8
#define GOODIX_READY_FLAG       0x80
#define GOODIX_MAX_POINTS             5
#define GOODIX_RESET_GPIO           17
#define GOODIX_INT_GPIO              4

#define FT5X06_REG_TD_STATUS      0x02
#define FT5X06_REG_P1_XH          0x03
#define FT5X06_REG_P1_XL          0x04
#define FT5X06_REG_P1_YH          0x05
#define FT5X06_REG_P1_YL          0x06
#define FT5X06_REG_ID             0xa3
#define FT5X06_REG_FW_VER         0xa6
#define FT5X06_MAX_POINTS            5
#define FT5X06_EVENT_DOWN         0x00
#define FT5X06_EVENT_UP           0x01
#define FT5X06_EVENT_CONTACT      0x02

#define WAVESHARE_REG_TP           0x94
#define WAVESHARE_REG_LCD          0x95

#define WAVESHARE_GPIO_AVDD         0
#define WAVESHARE_GPIO_PANEL_RESET  1
#define WAVESHARE_GPIO_ENABLE       2
#define WAVESHARE_GPIO_IOVCC        4
#define WAVESHARE_GPIO_VCC          8
#define WAVESHARE_GPIO_TS_RESET     9

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

typedef struct {
	int32_t bus;
	uint32_t sda;
	uint32_t scl;
	const char* name;
} dsi_i2c_bus_t;

typedef enum {
	TOUCH_KIND_NONE = 0,
	TOUCH_KIND_GOODIX = 1,
	TOUCH_KIND_FT5X06 = 2,
} touch_kind_t;

static const dsi_i2c_bus_t dsi_i2c_buses[] = {
	{4, 40, 41, "i2c4-dsi"},
	{1,  2,  3, "i2c1-header"},
};

static bool press = false;
static tp_point_t point[GOODIX_MAX_POINTS];
static uint8_t point_nr = 0;
static uint64_t last_ts = 0;
static uint16_t touch_data[TP_TOUCH_CACHE_SIZE][3];
static uint32_t touch_data_read = 0;
static uint32_t touch_data_write = 0;
static bool tp_ready = false;
static uint32_t poll_sleep_us = TP_POLL_MIN_US;
static uint32_t i2c_fail_count = 0;
static uint64_t tp_retry_ts = 0;
static uint32_t tp_retry_count = 0;
static bool touch_release_hint = false;
static bool last_emit_valid = false;
static uint16_t last_emit_state = 0;
static uint16_t last_emit_x = 0;
static uint16_t last_emit_y = 0;
static uint8_t touch_addr = GOODIX_ADDR_PRIMARY;
static touch_kind_t touch_kind = TOUCH_KIND_NONE;
static const dsi_i2c_bus_t* active_bus = NULL;
static bool waveshare_panel_reset_tried = false;
static uint16_t waveshare_power_state = 0;
static bool waveshare_power_state_valid = false;

static tp_status_t goodix_probe_candidates(const dsi_i2c_bus_t* bus);
static tp_status_t ft5x06_probe(const dsi_i2c_bus_t* bus);

static bool touch_has_data(void) {
	return (touch_data_write - touch_data_read) > 0;
}

static void touch_push(uint16_t state, uint16_t x, uint16_t y) {
	uint16_t* evt;

	if (touch_data_write - touch_data_read >= TP_TOUCH_CACHE_SIZE)
		touch_data_read++;

	evt = touch_data[touch_data_write % TP_TOUCH_CACHE_SIZE];
	evt[0] = state;
	evt[1] = x;
	evt[2] = y;
	touch_data_write++;
}

static bool touch_emit(uint16_t state, uint16_t x, uint16_t y) {
	if (last_emit_valid &&
			last_emit_state == state &&
			last_emit_x == x &&
			last_emit_y == y)
		return false;

	touch_push(state, x, y);
	last_emit_valid = true;
	last_emit_state = state;
	last_emit_x = x;
	last_emit_y = y;
	return true;
}

static bool touch_release_if_pressed(void) {
	bool emitted;

	if (!press)
		return false;

	press = false;
	emitted = touch_emit(0, point[0].x, point[0].y);
	return emitted;
}

static int32_t dsi_i2c_select(const dsi_i2c_bus_t* bus) {
	active_bus = bus;
	if (bcm2712_i2c_init_pins(bus->bus, bus->sda, bus->scl) != 0)
		return -1;
	usleep(2000);
	return 0;
}

static uint32_t dsi_i2c_write_reg16(uint8_t addr, uint16_t reg,
		const uint8_t* data, uint16_t len) {
	uint8_t buf[258];
	uint16_t i;

	if (active_bus == NULL || len > sizeof(buf) - 2)
		return 1;
	buf[0] = (uint8_t)(reg >> 8);
	buf[1] = (uint8_t)(reg & 0xff);
	for (i = 0; i < len; i++)
		buf[i + 2] = data[i];
	return bcm2712_i2c_write(active_bus->bus, addr, buf, len + 2) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_reg16(uint8_t addr, uint16_t reg,
		uint8_t* data, uint16_t len) {
	uint8_t reg_buf[2];

	if (active_bus == NULL)
		return 1;
	reg_buf[0] = (uint8_t)(reg >> 8);
	reg_buf[1] = (uint8_t)(reg & 0xff);
	return bcm2712_i2c_write_read(active_bus->bus, addr,
			reg_buf, sizeof(reg_buf), data, len) == 0 ? 0 : 1;
}

/* like read_reg16, but with a full stop between the two phases; some
 * controllers NAK the repeated-start form while busy */
static uint32_t dsi_i2c_read_reg16_split(uint8_t addr, uint16_t reg,
		uint8_t* data, uint16_t len) {
	uint8_t reg_buf[2];

	if (active_bus == NULL)
		return 1;
	reg_buf[0] = (uint8_t)(reg >> 8);
	reg_buf[1] = (uint8_t)(reg & 0xff);
	if (bcm2712_i2c_write(active_bus->bus, addr, reg_buf, sizeof(reg_buf)) != 0)
		return 1;
	usleep(TP_I2C_SPLIT_DELAY_US);
	return bcm2712_i2c_read(active_bus->bus, addr, data, len) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_write_reg8(uint8_t addr, uint8_t reg, uint8_t value) {
	if (active_bus == NULL)
		return 1;
	return bcm2712_i2c_putb(active_bus->bus, addr, reg, value) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_reg8(uint8_t addr, uint8_t reg, uint8_t* value) {
	if (active_bus == NULL)
		return 1;
	return bcm2712_i2c_write_read(active_bus->bus, addr,
			&reg, 1, value, 1) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_reg8_split(uint8_t addr, uint8_t reg, uint8_t* value) {
	if (active_bus == NULL)
		return 1;
	if (bcm2712_i2c_write(active_bus->bus, addr, &reg, 1) != 0)
		return 1;
	usleep(TP_I2C_SPLIT_DELAY_US);
	return bcm2712_i2c_read(active_bus->bus, addr, value, 1) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_regs8(uint8_t addr, uint8_t reg,
		uint8_t* data, uint16_t len) {
	if (len == 0)
		return 0;
	if (active_bus == NULL)
		return 1;
	return bcm2712_i2c_write_read(active_bus->bus, addr,
			&reg, 1, data, len) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_regs8_split(uint8_t addr, uint8_t reg,
		uint8_t* data, uint16_t len) {
	if (active_bus == NULL)
		return 1;
	if (bcm2712_i2c_write(active_bus->bus, addr, &reg, 1) != 0)
		return 1;
	usleep(TP_I2C_SPLIT_DELAY_US);
	return bcm2712_i2c_read(active_bus->bus, addr, data, len) == 0 ? 0 : 1;
}

static uint32_t dsi_i2c_read_reg8_retry(uint8_t addr, uint8_t reg, uint8_t* value) {
	uint32_t attempt;

	for (attempt = 0; attempt < TP_I2C_RETRY_MAX; attempt++) {
		if (dsi_i2c_read_reg8(addr, reg, value) == 0)
			return 0;
		if (dsi_i2c_read_reg8_split(addr, reg, value) == 0)
			return 0;
		if (active_bus == NULL)
			break;
		usleep(2000);
		if (dsi_i2c_select(active_bus) != 0)
			break;
		usleep(2000);
	}
	return 1;
}

static uint32_t dsi_i2c_read_regs8_retry(uint8_t addr, uint8_t reg,
		uint8_t* data, uint16_t len) {
	uint32_t attempt;

	for (attempt = 0; attempt < TP_I2C_RETRY_MAX; attempt++) {
		if (dsi_i2c_read_regs8(addr, reg, data, len) == 0)
			return 0;
		if (dsi_i2c_read_regs8_split(addr, reg, data, len) == 0)
			return 0;
		if (active_bus == NULL)
			break;
		usleep(2000);
		if (dsi_i2c_select(active_bus) != 0)
			break;
		usleep(2000);
	}
	return 1;
}

static bool waveshare_mcu_bus(const dsi_i2c_bus_t* bus) {
	if (bus == NULL)
		return false;
	/* the panel MCU sits on the DSI FPC ribbon only */
	return bus->bus == 4;
}

static bool waveshare_mcu_present(const dsi_i2c_bus_t* bus) {
	uint8_t v = 0;

	if (!waveshare_mcu_bus(bus))
		return false;
	return dsi_i2c_read_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_LCD, &v) == 0;
}

static int32_t waveshare_mcu_write_power(uint16_t state, uint32_t settle_ms) {
	if (dsi_i2c_write_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_TP,
				(uint8_t)(state >> 8)) != 0)
		return -1;
	if (dsi_i2c_write_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_LCD,
				(uint8_t)(state & 0xff)) != 0)
		return -1;
	usleep(settle_ms * 1000u);
	return 0;
}

/* read back both power registers so we never clobber the rail bits the
 * display daemon (dsi_fbdisplayd) has already switched on */
static int32_t waveshare_mcu_sync_power(void) {
	uint8_t tp = 0;
	uint8_t lcd = 0;

	if (dsi_i2c_read_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_TP, &tp) != 0)
		return -1;
	if (dsi_i2c_read_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_LCD, &lcd) != 0)
		return -1;

	waveshare_power_state = (uint16_t)(((uint16_t)tp << 8) | lcd);
	waveshare_power_state_valid = true;
	return 0;
}

static int32_t waveshare_mcu_set_gpio(uint8_t gpio, bool level,
		uint32_t settle_ms) {
	uint16_t bit = (uint16_t)(1u << gpio);

	if (!waveshare_power_state_valid && waveshare_mcu_sync_power() != 0)
		return -1;

	if (level)
		waveshare_power_state |= bit;
	else
		waveshare_power_state &= (uint16_t)~bit;

	return waveshare_mcu_write_power(waveshare_power_state, settle_ms);
}

static tp_status_t ft5x06_probe(const dsi_i2c_bus_t* bus) {
	uint8_t chip_id = 0xff;
	uint8_t fw_ver = 0xff;

	(void)bus;

	if (dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_ID, &chip_id) != 0) {
		if (dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_FW_VER, &fw_ver) != 0)
			return TP_NOT_RESPONSE;
	}
	else {
		(void)dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_FW_VER, &fw_ver);
	}

	touch_addr = FT5X06_ADDR;
	touch_kind = TOUCH_KIND_FT5X06;
	return TP_OK;
}

static tp_status_t waveshare_mcu_probe_touch_sequence(const dsi_i2c_bus_t* bus) {
	if (!waveshare_mcu_present(bus))
		return TP_NOT_RESPONSE;

	if (waveshare_mcu_sync_power() != 0)
		return TP_NOT_RESPONSE;

	if (goodix_probe_candidates(bus) == TP_OK)
		return TP_OK;

	if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_VCC, true, 20u) != 0)
		return TP_NOT_RESPONSE;
	if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_ENABLE, true, 20u) != 0)
		return TP_NOT_RESPONSE;
	if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_AVDD, true, 40u) != 0)
		return TP_NOT_RESPONSE;
	if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_IOVCC, true, 60u) != 0)
		return TP_NOT_RESPONSE;
	if (goodix_probe_candidates(bus) == TP_OK)
		return TP_OK;

	if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_TS_RESET, true, 20u) != 0)
		return TP_NOT_RESPONSE;
	if (goodix_probe_candidates(bus) == TP_OK)
		return TP_OK;

	if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_TS_RESET, false, 120u) != 0)
		return TP_NOT_RESPONSE;
	if (goodix_probe_candidates(bus) == TP_OK)
		return TP_OK;

	/* full panel reset disturbs the display, only try it once */
	if (!waveshare_panel_reset_tried) {
		waveshare_panel_reset_tried = true;
		if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_PANEL_RESET, true, 20u) != 0)
			return TP_NOT_RESPONSE;
		if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_PANEL_RESET, false, 180u) != 0)
			return TP_NOT_RESPONSE;
		if (goodix_probe_candidates(bus) == TP_OK)
			return TP_OK;

		if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_TS_RESET, true, 20u) != 0)
			return TP_NOT_RESPONSE;
		if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_TS_RESET, false, 120u) != 0)
			return TP_NOT_RESPONSE;
		if (goodix_probe_candidates(bus) == TP_OK)
			return TP_OK;
	}

	return TP_NOT_RESPONSE;
}

/* GT9xx address selection via RESET/INT, only possible when the
 * controller's reset lines sit on the 40-pin header (bus i2c1) */
static void goodix_reset_select(const dsi_i2c_bus_t* bus, uint8_t addr) {
	bool int_level;

	if (bus == NULL)
		return;
	if (bus->sda != 2 || bus->scl != 3)
		return;
	if (addr != GOODIX_ADDR_PRIMARY && addr != GOODIX_ADDR_FALLBACK)
		return;

	int_level = (addr == GOODIX_ADDR_FALLBACK);

	bcm2712_gpio_init();
	bcm2712_gpio_config(GOODIX_RESET_GPIO, GPIO_FUNC_OUTPUT);
	bcm2712_gpio_config(GOODIX_INT_GPIO, GPIO_FUNC_OUTPUT);
	bcm2712_gpio_pull(GOODIX_INT_GPIO, GPIO_PULL_NONE);

	bcm2712_gpio_write(GOODIX_INT_GPIO, int_level);

	bcm2712_gpio_write(GOODIX_RESET_GPIO, false);
	usleep(20000);
	bcm2712_gpio_write(GOODIX_RESET_GPIO, true);
	usleep(60000);

	bcm2712_gpio_config(GOODIX_INT_GPIO, GPIO_FUNC_INPUT);
	bcm2712_gpio_pull(GOODIX_INT_GPIO, GPIO_PULL_UP);
	usleep(20000);
}

static bool goodix_id_valid(const uint8_t* id_buf) {
	if ((id_buf[0] == 0 && id_buf[1] == 0 &&
				id_buf[2] == 0 && id_buf[3] == 0) ||
			(id_buf[0] == 0xff && id_buf[1] == 0xff &&
			 id_buf[2] == 0xff && id_buf[3] == 0xff))
		return false;
	return true;
}

static tp_status_t goodix_write_reg(uint16_t reg, const uint8_t* data, uint16_t len) {
	return dsi_i2c_write_reg16(touch_addr, reg, data, len) == 0 ?
		TP_OK : TP_NOT_RESPONSE;
}

static tp_status_t goodix_read_reg(uint16_t reg, uint8_t* data, uint16_t len) {
	if (dsi_i2c_read_reg16(touch_addr, reg, data, len) == 0)
		return TP_OK;
	if (waveshare_mcu_bus(active_bus) &&
			dsi_i2c_read_reg16_split(touch_addr, reg, data, len) == 0)
		return TP_OK;
	return TP_NOT_RESPONSE;
}

static tp_status_t goodix_set_command(uint8_t value) {
	return goodix_write_reg(GOODIX_REG_COMMAND, &value, 1);
}

static tp_status_t goodix_probe_addr(uint8_t addr, uint8_t* id_buf) {
	tp_status_t ret;

	touch_addr = addr;
	touch_kind = TOUCH_KIND_GOODIX;
	ret = goodix_read_reg(GOODIX_REG_ID, id_buf, 4);
	return ret;
}

static tp_status_t goodix_probe_candidates(const dsi_i2c_bus_t* bus) {
	static const uint8_t candidates_default[] = {
		GOODIX_ADDR_PRIMARY,
		GOODIX_ADDR_FALLBACK,
	};
	static const uint8_t candidates_waveshare[] = {
		GOODIX_ADDR_FALLBACK,
		GOODIX_ADDR_PRIMARY,
	};
	uint8_t id_buf[4];
	const uint8_t* candidates;
	uint32_t candidate_nr;
	uint32_t addr_i;

	if (waveshare_mcu_bus(bus)) {
		candidates = candidates_waveshare;
		candidate_nr = sizeof(candidates_waveshare);
	}
	else {
		candidates = candidates_default;
		candidate_nr = sizeof(candidates_default);
	}

	for (addr_i = 0; addr_i < candidate_nr; addr_i++) {
		goodix_reset_select(bus, candidates[addr_i]);
		memset(id_buf, 0, sizeof(id_buf));
		if (goodix_probe_addr(candidates[addr_i], id_buf) != TP_OK)
			continue;
		if (!goodix_id_valid(id_buf))
			continue;

		goodix_set_command(0x00);
		return TP_OK;
	}

	return TP_NOT_RESPONSE;
}

static tp_status_t touch_init(void) {
	uint32_t bus_i;

	for (bus_i = 0; bus_i < sizeof(dsi_i2c_buses) / sizeof(dsi_i2c_buses[0]); bus_i++) {
		if (dsi_i2c_select(&dsi_i2c_buses[bus_i]) != 0)
			continue;
		usleep(20000);

		if (ft5x06_probe(&dsi_i2c_buses[bus_i]) == TP_OK)
			return TP_OK;
		if (waveshare_mcu_probe_touch_sequence(&dsi_i2c_buses[bus_i]) == TP_OK)
			return TP_OK;
		if (goodix_probe_candidates(&dsi_i2c_buses[bus_i]) == TP_OK)
			return TP_OK;
	}

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

static tp_status_t ft5x06_read_touch(tp_point_t* pts, uint8_t* nr) {
	uint8_t buf[5];
	uint8_t count;
	uint8_t event;

	touch_release_hint = false;

	if (dsi_i2c_read_regs8_retry(FT5X06_ADDR, FT5X06_REG_TD_STATUS,
					buf, sizeof(buf)) != 0) {
		if (dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_TD_STATUS, &buf[0]) != 0 ||
				dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_P1_XH, &buf[1]) != 0 ||
				dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_P1_XL, &buf[2]) != 0 ||
				dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_P1_YH, &buf[3]) != 0 ||
				dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_P1_YL, &buf[4]) != 0)
			return TP_NOT_RESPONSE;
	}

	count = (uint8_t)(buf[0] & 0x0f);
	if (count > FT5X06_MAX_POINTS)
		count = FT5X06_MAX_POINTS;
	if (count == 0) {
		*nr = 0;
		return TP_OK;
	}

	event = (uint8_t)(buf[1] >> 6);
	if (event != FT5X06_EVENT_DOWN && event != FT5X06_EVENT_CONTACT) {
		*nr = 0;
		if (event == FT5X06_EVENT_UP)
			touch_release_hint = true;
		return event == FT5X06_EVENT_UP ? TP_OK : TP_NO_DATA;
	}

	pts[0].x = (uint16_t)(((uint16_t)(buf[1] & 0x0f) << 8) | buf[2]);
	pts[0].y = (uint16_t)(((uint16_t)(buf[3] & 0x0f) << 8) | buf[4]);
	*nr = 1;
	return TP_OK;
}

static tp_status_t tp_read_touch(tp_point_t* pts, uint8_t* nr) {
	if (touch_kind == TOUCH_KIND_FT5X06)
		return ft5x06_read_touch(pts, nr);
	return goodix_read_touch(pts, nr);
}

static int tp_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)offset;
	(void)p;

	if (!touch_has_data())
		return VFS_ERR_RETRY;
	if (size < 6)
		return -1;

	memcpy(buf, touch_data[touch_data_read % TP_TOUCH_CACHE_SIZE], 6);
	memset(touch_data[touch_data_read % TP_TOUCH_CACHE_SIZE], 0,
			sizeof(touch_data[0]));
	touch_data_read++;
	return 6;
}

static uint32_t tp_check_poll_events(vdevice_t* dev, int fd, int from_pid,
		fsinfo_t* node, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)p;

	return touch_has_data() ? VFS_EVT_RD : 0;
}

static int tp_loop(vdevice_t* dev, void* p) {
	tp_status_t ret;
	bool need_wakeup = false;
	uint64_t now_ms = kernel_tic_ms(0);

	(void)p;

	if (!tp_ready) {
		if (now_ms >= tp_retry_ts) {
			tp_retry_count++;
			if (touch_init() == TP_OK) {
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
	touch_release_hint = false;
	ret = tp_read_touch(point, &point_nr);

	if (ret == TP_NOT_RESPONSE) {
		i2c_fail_count++;
		if (i2c_fail_count >= TP_I2C_FAIL_MAX) {
			i2c_fail_count = 0;
			need_wakeup = touch_release_if_pressed() || need_wakeup;
			tp_ready = false;
			tp_retry_ts = now_ms;
			slog("dsi_touchd: link_lost bus=%s addr=0x%02x kind=%s\n",
					active_bus == NULL ? "?" : active_bus->name,
					touch_addr,
					touch_kind == TOUCH_KIND_FT5X06 ? "ft5x06" : "goodix");
		}
	}
	else if (ret == TP_OK || ret == TP_NO_DATA) {
		i2c_fail_count = 0;
	}

	if (press && touch_release_hint) {
		need_wakeup = touch_release_if_pressed() || need_wakeup;
	}
	else if ((ret == TP_OK && !point_nr) || ret == TP_NO_DATA || ret == TP_NOT_RESPONSE) {
		if (press && (now_ms - last_ts) > TP_RELEASE_TIMEOUT_MS)
			need_wakeup = touch_release_if_pressed() || need_wakeup;
	}
	else if (ret == TP_OK && point_nr) {
		last_ts = now_ms;
		press = true;
		if (touch_emit((uint16_t)press, point[0].x, point[0].y))
			need_wakeup = true;
	}

	if (need_wakeup) {
		poll_sleep_us = TP_POLL_MIN_US;
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	}
	else if (press) {
		poll_sleep_us = TP_POLL_MIN_US;
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

	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "dsi_touch");
	dev.read = tp_read;
	dev.loop_step = tp_loop;
	dev.check_poll_events = tp_check_poll_events;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
