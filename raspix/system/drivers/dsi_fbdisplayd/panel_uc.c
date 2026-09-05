#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <arch/bcm283x/dsi1.h>
#include <arch/bcm283x/gpio.h>
#include <arch/bcm283x/i2c.h>

#include "panel_uc.h"

/* ---------------- Family table ---------------- */

/*
 * Modes transcribed from the ClockworkPi kernel drivers
 * (panel-cwu50.c / panel-cwd686.c :: default_mode).  The nominal pixel
 * clock is what the vendor declares; the shared clock bring-up turns it
 * into the HS bit clock and pays the integer-PLL rounding back out of
 * hfp, exactly like vc4_dsi_bridge_mode_fixup().
 *
 *   cwu50   clock 62500 kHz, htotal 803,  vtotal 1306
 *   cwd686  clock 54465 kHz, htotal 694,  vtotal 1308
 *
 * Both are 4-lane RGB888 with a continuous HS clock (the standalone
 * uConsole daemon set PHYC_HS_CLK_CONTINUOUS unconditionally).
 */
static const bcm283x_dsi1_mode_t _mode_cwu50 = {
	.width  = 720,
	.height = 1280,
	.hfp = 43,  .hsw = 20, .hbp = 20,
	.vfp = 8,   .vsw = 2,  .vbp = 16,
	.pixel_clock_hz = 62500000U,
	.lanes = 4,
	.continuous_clock = 1,
};

static const bcm283x_dsi1_mode_t _mode_cwd686 = {
	.width  = 480,
	.height = 1280,
	.hfp = 150, .hsw = 24, .hbp = 40,
	.vfp = 12,  .vsw = 6,  .vbp = 10,
	.pixel_clock_hz = 54465000U,
	.lanes = 4,
	.continuous_clock = 1,
};

int uc_panel_from_name(const char* name) {
	if (name == NULL)
		return -1;
	if (strcmp(name, "cwu50") == 0)
		return UC_PANEL_CWU50;
	if (strcmp(name, "cwu50_old") == 0)
		return UC_PANEL_CWU50OLD;
	if (strcmp(name, "cwd686") == 0)
		return UC_PANEL_CWD686;
	return -1;
}

const char* uc_panel_name(int which) {
	switch (which) {
	case UC_PANEL_CWD686:   return "cwd686";
	case UC_PANEL_CWU50OLD: return "cwu50_old";
	default:                return "cwu50";
	}
}

const bcm283x_dsi1_mode_t* uc_panel_mode(int which) {
	/* cwu50_old is the original hardware batch of the same glass: the
	 * two batches carried byte-identical timings upstream and differ
	 * only in the DDIC init table, so they share _mode_cwu50. */
	return which == UC_PANEL_CWD686 ? &_mode_cwd686 : &_mode_cwu50;
}

uint32_t uc_panel_hs_clock(int which) {
	return which == UC_PANEL_CWD686 ? 326790000U : 375000000U;
}

/* ---------------- DCS transport ---------------- */

/*
 * Both vendor tables are played back in HS command mode: neither
 * panel-*.c sets MIPI_DSI_MODE_LPM, so upstream never passes
 * MIPI_DSI_MSG_USE_LPM and TXPKT1C_CMD_MODE_LP stays clear.
 *
 * The transport is panel_uc_dsi.c's transcription of
 * fbdisplay6d/uc_dsi.c, not the shared library's cmd_write: it handles
 * the same short/long split and the >16-byte command-FIFO/pixel-FIFO
 * walk, but without the per-write gen-detection syscall the shared
 * accessor layer puts in front of every DSI1 store.
 */
int uc_dcs_write(uint8_t data_type, const uint8_t* payload, uint32_t len) {
	return uc_dsi_dcs_write(data_type, payload, len);
}

int uc_panel_init_table(int which) {
	switch (which) {
	case UC_PANEL_CWD686:   return uc_cwd686_init();
	case UC_PANEL_CWU50OLD: return uc_cwu50old_init();
	default:                return uc_cwu50_init();
	}
}

/* ---------------- AXP223 panel rails ---------------- */

/*
 * AXP223 register map, from Linux axp20x-regulator.c (AXP22X descs):
 *   ALDO1  vol 0x28 [4:0], en 0x10 bit6   ("audio-vdd")
 *   ALDO2  vol 0x29 [4:0], en 0x10 bit7   ("display-vcc"  <-- panel!)
 *   DLDO2  vol 0x16 [4:0], en 0x12 bit4
 *   DLDO3  vol 0x17 [4:0], en 0x12 bit5
 *   DLDO4  vol 0x18 [4:0], en 0x12 bit6
 * LDO range 700..3300mV in 100mV steps: 3300mV => sel 26 (0x1A).
 *
 * The bus is the software (bit-banged) I2C on GPIO0/GPIO1 that the
 * uConsole powerd uses, NOT the BSC0/BSC1 hardware controllers the
 * ws/rpi7 families talk to — so enabling these rails never disturbs a
 * panel MCU probe on the other families.
 */
#define AXP_ADDR            0x34U
#define AXP_PWR_OUT_CTRL1   0x10U
#define AXP_PWR_OUT_CTRL2   0x12U
#define AXP_ALDO1_V_OUT     0x28U
#define AXP_ALDO2_V_OUT     0x29U
#define AXP_DLDO2_V_OUT     0x16U
#define AXP_DLDO3_V_OUT     0x17U
#define AXP_DLDO4_V_OUT     0x18U
#define AXP_SEL_3V3         0x1AU

#define CTRL1_ALDO1_EN      (1U << 6)
#define CTRL1_ALDO2_EN      (1U << 7)
#define CTRL2_DLDO2_EN      (1U << 4)
#define CTRL2_DLDO3_EN      (1U << 5)
#define CTRL2_DLDO4_EN      (1U << 6)

#define AXP_SDA_GPIO        0
#define AXP_SCL_GPIO        1

static int _axp_try_once(void) {
	uint8_t v;

	/* Rail voltages first, then the enable bits (regulator core order). */
	i2c_putb(AXP_ADDR, AXP_ALDO1_V_OUT, AXP_SEL_3V3);
	i2c_putb(AXP_ADDR, AXP_ALDO2_V_OUT, AXP_SEL_3V3);
	i2c_putb(AXP_ADDR, AXP_DLDO2_V_OUT, AXP_SEL_3V3);
	i2c_putb(AXP_ADDR, AXP_DLDO3_V_OUT, AXP_SEL_3V3);
	i2c_putb(AXP_ADDR, AXP_DLDO4_V_OUT, AXP_SEL_3V3);

	v = i2c_getb(AXP_ADDR, AXP_PWR_OUT_CTRL1);
	i2c_putb(AXP_ADDR, AXP_PWR_OUT_CTRL1,
			v | CTRL1_ALDO1_EN | CTRL1_ALDO2_EN);
	v = i2c_getb(AXP_ADDR, AXP_PWR_OUT_CTRL2);
	i2c_putb(AXP_ADDR, AXP_PWR_OUT_CTRL2,
			v | CTRL2_DLDO2_EN | CTRL2_DLDO3_EN | CTRL2_DLDO4_EN);

	/* Verify the display rail actually latched. */
	v = i2c_getb(AXP_ADDR, AXP_PWR_OUT_CTRL1);
	if ((v & CTRL1_ALDO2_EN) == 0)
		return -1;
	v = i2c_getb(AXP_ADDR, AXP_PWR_OUT_CTRL2);
	if ((v & (CTRL2_DLDO2_EN | CTRL2_DLDO3_EN | CTRL2_DLDO4_EN)) !=
			(CTRL2_DLDO2_EN | CTRL2_DLDO3_EN | CTRL2_DLDO4_EN))
		return -1;
	return 0;
}

static void _axp_display_power(void) {
	int attempt;

	bcm283x_gpio_init();
	i2c_init(AXP_SDA_GPIO, AXP_SCL_GPIO);

	/*
	 * powerd polls the same bus every 300ms from another process; a
	 * collided transaction just reads back wrong, so retry a few
	 * times rather than trusting a single pass.
	 */
	for (attempt = 0; attempt < 5; attempt++) {
		if (_axp_try_once() == 0) {
			/* Let the panel rails rise + panel logic settle.
			 * This also covers the JD9365DA-H3 tRPWIRES gap
			 * between rails up and the reset pulse. */
			bcm283x_dsi1_mdelay(20);
			return;
		}
		bcm283x_dsi1_mdelay(5);
	}
	/*
	 * Not fatal: the PMIC may already hold the rails on from its own
	 * defaults, in which case the DCS table is the real verdict.
	 */
	printf("panel_uc: AXP223 display rails did not verify (addr 0x%02x)\n",
			AXP_ADDR);
}

/* ---------------- GPIO 8 reset ---------------- */

/*
 * Both DT overlays declare `reset-gpio = <&gpio 8 1>` — flag 1 is
 * GPIO_ACTIVE_LOW, so every gpiod logical value in panel-cwu50.c is
 * INVERTED on the physical pin:
 *
 *   probe:   devm_gpiod_get_optional(..., GPIOD_OUT_HIGH)
 *            -> logical 1 -> physical LOW
 *   prepare: gpiod_set_value(reset, 0) -> physical HIGH, msleep(10)
 *            gpiod_set_value(reset, 1) -> physical LOW,  msleep(120)
 *            ... DCS init follows, pin stays physical LOW while running.
 *
 * Getting this backwards holds the panel in reset for ever: backlight
 * (GPIO 9) still lights, but no DCS command is ever accepted.
 * panel-cwd686.c uses the exact same pulse (10ms assert, 120ms settle).
 */
static void _reset_pin_claim(void) {
	bcm283x_gpio_init();

	/*
	 * GPIO 8 defaults to SPI0_CE0 alt-func on CM4; force it back to a
	 * plain output driven by us, at its probe-time level.
	 */
	bcm283x_gpio_pull(UC_PANEL_RESET_GPIO, GPIO_PULL_NONE);
	bcm283x_gpio_config(UC_PANEL_RESET_GPIO, GPIO_OUTPUT);
	bcm283x_gpio_write(UC_PANEL_RESET_GPIO, 0);
}

void uc_panel_reset_pulse(void) {
	_reset_pin_claim();

	bcm283x_gpio_write(UC_PANEL_RESET_GPIO, 1);   /* gpiod 0 -> phys HIGH */
	bcm283x_dsi1_mdelay(10);
	bcm283x_gpio_write(UC_PANEL_RESET_GPIO, 0);   /* gpiod 1 -> phys LOW  */
	bcm283x_dsi1_mdelay(120);
}

/* ---------------- OCP8178 backlight (GPIO 9) ---------------- */

/*
 * Timings straight out of drivers/video/backlight/ocp8178_bl.c in the
 * ClockworkPi Linux tree.  The chip enters a 1-wire configuration mode
 * after a defined level sequence, then takes an address byte (0x72)
 * followed by a 5-bit brightness code.
 */
#define OCP_DETECT_DELAY_US       200U    /* DETECT_DELAY */
#define OCP_DETECT_TIME_US        500U    /* DETECT_TIME */
#define OCP_DETECT_WINDOW_US     1000U    /* DETECT_WINDOW_TIME */
#define OCP_SHUTDOWN_MS             3U    /* SHUTDOWN_TIME (=3000us) */
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

static int _bl_ready = 0;

static inline void _bl_gpio_set(int v) {
	bcm283x_gpio_write(UC_BACKLIGHT_GPIO, v);
}

static void _bl_entry_1wire_mode(void) {
	_bl_gpio_set(0);
	bcm283x_dsi1_mdelay(OCP_SHUTDOWN_MS);
	_bl_gpio_set(1);
	bcm283x_dsi1_udelay(OCP_DETECT_DELAY_US);
	_bl_gpio_set(0);
	bcm283x_dsi1_udelay(OCP_DETECT_TIME_US);
	_bl_gpio_set(1);
	bcm283x_dsi1_udelay(OCP_DETECT_WINDOW_US);
}

static void _bl_write_bit(int bit) {
	if (bit) {
		_bl_gpio_set(0);
		bcm283x_dsi1_udelay(OCP_HIGH_BIT_LOW_TIME_US);
		_bl_gpio_set(1);
		bcm283x_dsi1_udelay(OCP_HIGH_BIT_HIGH_TIME_US);
	} else {
		_bl_gpio_set(0);
		bcm283x_dsi1_udelay(OCP_LOW_BIT_LOW_TIME_US);
		_bl_gpio_set(1);
		bcm283x_dsi1_udelay(OCP_LOW_BIT_HIGH_TIME_US);
	}
}

static void _bl_write_byte(uint8_t byte) {
	uint8_t data;
	int i;

	/* Address byte (0x72). */
	data = OCP_ADDR_BYTE;
	_bl_gpio_set(1);
	bcm283x_dsi1_udelay(OCP_START_TIME_US);
	for (i = 0; i < 8; ++i) {
		_bl_write_bit((data & 0x80) != 0);
		data = (uint8_t)(data << 1);
	}
	_bl_gpio_set(0);
	bcm283x_dsi1_udelay(OCP_END_TIME_US);

	/* Data byte: only bottom 5 bits are the brightness raw value. */
	data = (uint8_t)(byte & 0x1fU);
	_bl_gpio_set(1);
	bcm283x_dsi1_udelay(OCP_START_TIME_US);
	for (i = 0; i < 8; ++i) {
		_bl_write_bit((data & 0x80) != 0);
		data = (uint8_t)(data << 1);
	}
	_bl_gpio_set(0);
	bcm283x_dsi1_udelay(OCP_END_TIME_US);
	_bl_gpio_set(1);
}

void uc_backlight_init(void) {
	if (_bl_ready)
		return;
	bcm283x_gpio_init();

	/*
	 * GPIO 9 defaults to SPI0_MISO alt-func on CM4; force it back to a
	 * plain output before we start pulsing.
	 */
	bcm283x_gpio_pull(UC_BACKLIGHT_GPIO, GPIO_PULL_NONE);
	bcm283x_gpio_config(UC_BACKLIGHT_GPIO, GPIO_OUTPUT);
	_bl_gpio_set(1);
	_bl_ready = 1;
}

void uc_backlight_set(uint8_t level) {
	uint8_t raw;
	int i;

	if (!_bl_ready)
		uc_backlight_init();

	if (level > UC_BACKLIGHT_MAX_LEVEL)
		level = UC_BACKLIGHT_MAX_LEVEL;
	raw = _bl_table[level];

	/*
	 * The Linux driver writes twice; the second pass is a belt-and-
	 * braces retry that also recovers from the first pass timing being
	 * slightly off, which matters more here since we cannot disable
	 * interrupts around the pulses.
	 */
	for (i = 0; i < 2; ++i) {
		_bl_entry_1wire_mode();
		_bl_write_byte(raw);
	}
}

/* Backlight off = hold the line low past SHUTDOWN_TIME. */
static void _bl_off(void) {
	_bl_gpio_set(0);
	bcm283x_dsi1_mdelay(OCP_SHUTDOWN_MS + 2U);
}

void uc_backlight_blink(uint32_t n) {
	uint32_t i;

	if (!_bl_ready)
		uc_backlight_init();
	for (i = 0; i < n; ++i) {
		_bl_off();
		bcm283x_dsi1_mdelay(120);
		uc_backlight_set(UC_BACKLIGHT_DEFAULT);
		bcm283x_dsi1_mdelay(120);
	}
	/* Gap so consecutive blink groups can be told apart. */
	bcm283x_dsi1_mdelay(400);
}

/* ---------------- Early bring-up sequence ---------------- */

/*
 * Defined last so it can call the reset-pin, backlight and AXP helpers
 * above in the exact order the standalone uConsole daemon used:
 * claim reset -> backlight -> panel rails, all before any DSI PHY work.
 */
void uc_panel_prepare(void) {
	_reset_pin_claim();
	uc_backlight_init();
	uc_backlight_set(UC_BACKLIGHT_DEFAULT);
	_axp_display_power();
}
