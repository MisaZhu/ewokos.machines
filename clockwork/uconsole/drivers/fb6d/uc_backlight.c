#include "uc_backlight.h"
#include "uc_time.h"

#include <arch/bcm283x/gpio.h>

/*
 * Timings straight out of drivers/video/backlight/ocp8178_bl.c in the
 * ClockworkPi Linux tree. Comments below reflect the constants that
 * driver defines.
 */
#define OCP_DETECT_DELAY_US       200U    /* DETECT_DELAY */
#define OCP_DETECT_TIME_US        500U    /* DETECT_TIME */
#define OCP_DETECT_WINDOW_US     1000U    /* DETECT_WINDOW_TIME */
#define OCP_SHUTDOWN_MS             3U    /* SHUTDOWN_TIME (=3000μs) */
#define OCP_START_TIME_US          10U
#define OCP_END_TIME_US            10U
#define OCP_LOW_BIT_HIGH_TIME_US   10U
#define OCP_LOW_BIT_LOW_TIME_US    50U
#define OCP_HIGH_BIT_HIGH_TIME_US  50U
#define OCP_HIGH_BIT_LOW_TIME_US   10U

/* First byte of every write is the address (0x72). */
#define OCP_ADDR_BYTE  0x72U

/*
 * ocp8178_bl.c:
 *   unsigned char ocp8178_bl_table[MAX_BRIGHTNESS_VALUE+1] =
 *       { 0, 1, 4, 8, 12, 16, 20, 24, 28, 31 };
 */
static const uint8_t _bl_table[UC_BACKLIGHT_MAX_LEVEL + 1] = {
	0, 1, 4, 8, 12, 16, 20, 24, 28, 31,
};

static int _ready = 0;

static inline void _gpio_set(int v) {
	bcm283x_gpio_write(UC_BACKLIGHT_GPIO, v);
}

static void _entry_1wire_mode(void) {
	_gpio_set(0);
	uc_mdelay(OCP_SHUTDOWN_MS);
	_gpio_set(1);
	uc_udelay(OCP_DETECT_DELAY_US);
	_gpio_set(0);
	uc_udelay(OCP_DETECT_TIME_US);
	_gpio_set(1);
	uc_udelay(OCP_DETECT_WINDOW_US);
}

static void _write_bit(int bit) {
	if (bit) {
		_gpio_set(0);
		uc_udelay(OCP_HIGH_BIT_LOW_TIME_US);
		_gpio_set(1);
		uc_udelay(OCP_HIGH_BIT_HIGH_TIME_US);
	} else {
		_gpio_set(0);
		uc_udelay(OCP_LOW_BIT_LOW_TIME_US);
		_gpio_set(1);
		uc_udelay(OCP_LOW_BIT_HIGH_TIME_US);
	}
}

static void _write_byte(uint8_t byte) {
	uint8_t data;
	int i;

	/* Address byte (0x72). */
	data = OCP_ADDR_BYTE;
	_gpio_set(1);
	uc_udelay(OCP_START_TIME_US);
	for (i = 0; i < 8; ++i) {
		_write_bit((data & 0x80) != 0);
		data = (uint8_t)(data << 1);
	}
	_gpio_set(0);
	uc_udelay(OCP_END_TIME_US);

	/* Data byte: only bottom 5 bits are the brightness raw value. */
	data = (uint8_t)(byte & 0x1fU);
	_gpio_set(1);
	uc_udelay(OCP_START_TIME_US);
	for (i = 0; i < 8; ++i) {
		_write_bit((data & 0x80) != 0);
		data = (uint8_t)(data << 1);
	}
	_gpio_set(0);
	uc_udelay(OCP_END_TIME_US);
	_gpio_set(1);
}

void uc_backlight_init(void) {
	if (_ready) {
		return;
	}
	bcm283x_gpio_init();
	uc_time_init();

	/*
	 * GPIO 9 defaults to SPI0_MISO alt-func on CM4; force it back to a
	 * plain output before we start pulsing.
	 */
	bcm283x_gpio_pull(UC_BACKLIGHT_GPIO, GPIO_PULL_NONE);
	bcm283x_gpio_config(UC_BACKLIGHT_GPIO, GPIO_OUTPUT);
	_gpio_set(1);
	_ready = 1;
}

void uc_backlight_set(uint8_t level) {
	uint8_t raw;
	int i;

	if (!_ready) {
		uc_backlight_init();
	}

	if (level > UC_BACKLIGHT_MAX_LEVEL) {
		level = UC_BACKLIGHT_MAX_LEVEL;
	}
	raw = _bl_table[level];

	/*
	 * The Linux driver writes twice; the second pass is a belt-and-
	 * braces retry that also recovers from the first pass timing being
	 * slightly off, which matters more here since we cannot disable
	 * interrupts around the pulses.
	 */
	for (i = 0; i < 2; ++i) {
		_entry_1wire_mode();
		_write_byte(raw);
	}
}

/*
 * Backlight off = hold the line low past SHUTDOWN_TIME; on = run the
 * regular set path which re-enters 1-wire mode from shutdown.
 */
static void _bl_off(void) {
	_gpio_set(0);
	uc_mdelay(OCP_SHUTDOWN_MS + 2U);
}

static void _bl_on(void) {
	uc_backlight_set(UC_BACKLIGHT_DEFAULT);
}

void uc_backlight_blink(uint32_t n) {
	uint32_t i;

	if (!_ready) {
		uc_backlight_init();
	}
	for (i = 0; i < n; ++i) {
		_bl_off();
		uc_mdelay(250);
		_bl_on();
		uc_mdelay(250);
	}
	/* Gap so consecutive blink groups can be told apart. */
	uc_mdelay(1000);
}

void uc_backlight_panic(uint32_t n) {
	uint32_t i;

	if (!_ready) {
		uc_backlight_init();
	}
	for (;;) {
		for (i = 0; i < n; ++i) {
			_bl_off();
			uc_mdelay(120);
			_bl_on();
			uc_mdelay(120);
		}
		uc_mdelay(1500);
	}
}
