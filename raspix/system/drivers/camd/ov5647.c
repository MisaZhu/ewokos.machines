/*----------------------------------------------------------------------------*/
/**
 * OV5647 5MP CMOS sensor driver (I2C control via bit-bang)
 * - 16-bit register addresses
 * - OV5647 is a RAW Bayer sensor (no ISP): output is SBGGR8/SBGGR10 only.
 * - Mode: 640x480 RAW8 (SBGGR8) 2x2 binned, register table taken from the
 *   upstream Linux ov5647.c driver.
**/
/*----------------------------------------------------------------------------*/
#include <stdint.h>
#include <string.h>
#include <arch/bcm283x/i2c.h>
#include <arch/bcm283x/gpio.h>
#include <ewoksys/proc.h>
#include <ewoksys/klog.h>
#include "ov5647.h"

/*----------------------------------------------------------------------------*/
/* I2C helpers for 16-bit register address devices */

/* low-level bit-bang primitives (from libarch_bcm283x i2c.c) */
extern void i2c_do_start(void);
extern void i2c_do_stop(void);
extern uint32_t i2c_do_write_byte(uint8_t data);
extern uint8_t i2c_do_read_byte(int32_t ack);

static int32_t _sda_pin, _scl_pin;

/* I2C bus recovery: send 9 clocks to release stuck SDA */
static void i2c_bus_recovery(void) {
	int i;
	bcm283x_gpio_config(_scl_pin, GPIO_OUTPUT);
	bcm283x_gpio_config(_sda_pin, GPIO_INPUT);
	for (i = 0; i < 9; i++) {
		bcm283x_gpio_clr(_scl_pin);
		proc_usleep(5);
		bcm283x_gpio_set(_scl_pin);
		proc_usleep(5);
	}
	/* generate STOP to reset bus state */
	bcm283x_gpio_config(_sda_pin, GPIO_OUTPUT);
	bcm283x_gpio_clr(_sda_pin);
	proc_usleep(5);
	bcm283x_gpio_set(_scl_pin);
	proc_usleep(5);
	bcm283x_gpio_set(_sda_pin);
	proc_usleep(5);
}

static void ov5647_write_reg(uint16_t reg, uint8_t val) {
	uint8_t buf[3];
	buf[0] = (uint8_t)(reg >> 8);
	buf[1] = (uint8_t)(reg & 0xFF);
	buf[2] = val;
	i2c_puts_raw(OV5647_I2C_ADDR, buf, 3);
}

static uint8_t ov5647_read_reg(uint16_t reg) {
	uint8_t addr = (uint8_t)(OV5647_I2C_ADDR << 1);
	uint8_t val;

	i2c_do_start();
	i2c_do_write_byte(addr);         /* write mode */
	i2c_do_write_byte(reg >> 8);     /* reg high */
	i2c_do_write_byte(reg & 0xFF);   /* reg low */
	i2c_do_start();                  /* repeated START, no STOP */
	i2c_do_write_byte(addr | 0x01);  /* read mode */
	val = i2c_do_read_byte(0);       /* NACK, single byte */
	i2c_do_stop();
	return val;
}

/*----------------------------------------------------------------------------*/
/* Register table entry: {reg, val}, terminated by {0xFFFF, 0xFF} */

typedef struct {
	uint16_t reg;
	uint8_t val;
} reg_entry_t;

#define REG_END {0xFFFF, 0xFF}

/*----------------------------------------------------------------------------*/
/* 640x480 RAW8 (SBGGR8) full-FOV, 2-lane MIPI.
 * Geometry/timing aligned with the modern mainline Linux ov5647_640x480 mode
 * ("2x2 binned and subsampled down to VGA"): 0x3814/0x3815=0x35 gives the
 * full /4 reduction chain. The legacy 8-bit table used 0x31 (/2 only), so the
 * sensor's internal image stayed 1312x977 and the 640x480 output window cut a
 * ~2x zoomed top-left crop out of it - wrong FOV, off-center picture. */
static const reg_entry_t ov5647_640x480_raw8[] = {
	{0x0100, 0x00}, /* software standby */
	{0x0103, 0x01}, /* software reset */
	{0x3034, 0x08}, /* MIPI 8-bit mode */
	{0x3035, 0x21}, /* PLL sysclk div */
	{0x3036, 0x46}, /* PLL multiplier */
	{0x303C, 0x11}, /* PLL charge pump */
	{0x3106, 0xF5}, /* PLL clock select */
	{0x3821, 0x07}, /* horizontal binning + mirror */
	{0x3820, 0x41}, /* vertical binning */
	{0x3827, 0xEC},
	{0x370C, 0x03},
	{0x3612, 0x59},
	{0x3618, 0x00},
	{0x5000, 0x06}, /* lens correction off, BLC on */
	{0x5001, 0x01},
	{0x5002, 0x41},
	{0x5003, 0x08},
	{0x5A00, 0x08},
	{0x3000, 0x00}, /* IO direction: all input */
	{0x3001, 0x00},
	{0x3002, 0x00},
	{0x3016, 0x08},
	{0x3017, 0xE0},
	{0x3018, 0x44}, /* MIPI 2-lane mode */
	{0x301C, 0xF8},
	{0x301D, 0xF0},
	{0x3A18, 0x00}, /* AEC gain ceiling */
	{0x3A19, 0xF8},
	{0x3C01, 0x80}, /* band detection */
	{0x3B07, 0x0C},
	{0x380C, 0x07}, /* HTS = 1852 */
	{0x380D, 0x3C},
	{0x380E, 0x01}, /* VTS = 504 */
	{0x380F, 0xF8},
	{0x3814, 0x35}, /* x subsample: 2x2 binned + subsampled (/4) */
	{0x3815, 0x35}, /* y subsample: 2x2 binned + subsampled (/4) */
	{0x3708, 0x64},
	{0x3709, 0x52},
	{0x3808, 0x02}, /* x output size = 640 */
	{0x3809, 0x80},
	{0x380A, 0x01}, /* y output size = 480 */
	{0x380B, 0xE0},
	{0x3800, 0x00}, /* x start = 16 (inside active array, no black border) */
	{0x3801, 0x10},
	{0x3802, 0x00}, /* y start = 0 */
	{0x3803, 0x00},
	{0x3804, 0x0A}, /* x end = 2607 */
	{0x3805, 0x2F},
	{0x3806, 0x07}, /* y end = 1951 */
	{0x3807, 0x9F},
	/* Do not keep the VGA window centered inside a larger 648x488 envelope.
	 * camd/UNICAM allocates an exact 640x480 RAW8 buffer; the residual sensor
	 * offsets can leave wrapped edge lines in the captured frame. */
	{0x3811, 0x00}, /* x offset: exact 640-pixel output window */
	{0x3813, 0x00}, /* y offset: exact 480-line output window */
	{0x3630, 0x2E}, /* analog control */
	{0x3632, 0xE2},
	{0x3633, 0x23},
	{0x3634, 0x44},
	{0x3636, 0x06},
	{0x3620, 0x64},
	{0x3621, 0xE0},
	{0x3600, 0x37},
	{0x3704, 0xA0},
	{0x3703, 0x5A},
	{0x3715, 0x78},
	{0x3717, 0x01},
	{0x3731, 0x02},
	{0x370B, 0x60},
	{0x3705, 0x1A},
	{0x3F05, 0x02},
	{0x3F06, 0x10},
	{0x3F01, 0x0A},
	{0x3A08, 0x01}, /* AEC band steps */
	{0x3A09, 0x27},
	{0x3A0A, 0x00},
	{0x3A0B, 0xF6},
	{0x3A0D, 0x04},
	{0x3A0E, 0x03},
	{0x3A0F, 0x58}, /* AEC thresholds */
	{0x3A10, 0x50},
	{0x3A1B, 0x58},
	{0x3A1E, 0x50},
	{0x3A11, 0x60},
	{0x3A1F, 0x28},
	{0x4001, 0x02}, /* BLC start line */
	{0x4004, 0x02}, /* BLC line count */
	{0x4000, 0x09}, /* BLC enable */
	{0x4837, 0x24}, /* MIPI pclk period */
	{0x4050, 0x6E}, /* BLC ranges */
	{0x4051, 0x8F},
	{0x0100, 0x01}, /* exit standby */
	REG_END
};

/*----------------------------------------------------------------------------*/
/* write + readback verify with retry. The bit-bang I2C write path does not
 * check ACKs, so a register write can be dropped silently and randomly per
 * boot. A lost geometry register (e.g. y output size) makes the sensor emit
 * frames smaller than the UNICAM buffer window; the hardware then packs
 * several small frames back-to-back into one window - which is exactly the
 * "N stacked copies of the scene" picture (and it varies per boot). */
static int ov5647_write_reg_checked(uint16_t reg, uint8_t val) {
	int retry;
	for (retry = 0; retry < 3; retry++) {
		ov5647_write_reg(reg, val);
		proc_usleep(200);
		if (ov5647_read_reg(reg) == val)
			return 0;
		i2c_bus_recovery();
		proc_usleep(1000);
	}
	return -1;
}

/* program the table, verifying every register; returns the number of
 * registers that failed to stick (0 = fully applied) */
static int write_reg_table(const reg_entry_t* table) {
	int failed = 0;
	for (int i = 0; table[i].reg != 0xFFFF; i++) {
		uint16_t reg = table[i].reg;
		uint8_t val = table[i].val;
		if (reg == OV5647_REG_SW_RESET) {
			/* reset reads back 0; just write and settle - writes after a
			 * reset are silently lost without the delay */
			ov5647_write_reg(reg, val);
			proc_usleep(10000);
			continue;
		}
		if (ov5647_write_reg_checked(reg, val) != 0) {
			printf("ov5647: reg %04x=%02x failed to stick\n", reg, val);
			failed++;
		}
	}
	return failed;
}

/*----------------------------------------------------------------------------*/
int ov5647_init(int32_t sda_gpio, int32_t scl_gpio) {
	uint8_t id_hi, id_lo;
	int retry;

	_sda_pin = sda_gpio;
	_scl_pin = scl_gpio;

	i2c_init(sda_gpio, scl_gpio);
	i2c_set_wait_time(2); /* slow down bit-bang for sensor */
	proc_usleep(10000);

	/* bus recovery in case SDA is stuck low */
	i2c_bus_recovery();
	proc_usleep(10000);

	/* retry chip ID read */
	for (retry = 0; retry < 3; retry++) {
		id_hi = ov5647_read_reg(OV5647_REG_CHIPID_HI);
		id_lo = ov5647_read_reg(OV5647_REG_CHIPID_LO);
		if (id_hi == OV5647_CHIPID_HI && id_lo == OV5647_CHIPID_LO)
			break;
		printf("ov5647: retry %d, got %02x%02x\n", retry, id_hi, id_lo);
		i2c_bus_recovery();
		proc_usleep(50000);
	}

	if (id_hi != OV5647_CHIPID_HI || id_lo != OV5647_CHIPID_LO) {
		printf("ov5647: chip ID mismatch: %02x%02x (expect 5647)\n", id_hi, id_lo);
		return -1;
	}

	/* software reset */
	ov5647_write_reg(OV5647_REG_SW_RESET, 0x01);
	proc_usleep(5000);
	ov5647_write_reg(OV5647_REG_SW_RESET, 0x00);
	proc_usleep(5000);

	return 0;
}

/*----------------------------------------------------------------------------*/
/* verify the geometry + key config registers landed; returns 0 if ok.
 * 3 registers are not enough: an unapplied y-size (0x380A/0x380B) alone
 * produces the stacked-copies picture while passing a shallow verify. */
static int verify_mode_regs(void) {
	static const reg_entry_t expect[] = {
		{0x0100, 0x01},               /* streaming */
		{0x3808, 0x02}, {0x3809, 0x80}, /* x output size = 640 */
		{0x380A, 0x01}, {0x380B, 0xE0}, /* y output size = 480 */
		{0x380C, 0x07}, {0x380D, 0x3C}, /* HTS = 1852 */
		{0x380E, 0x01}, {0x380F, 0xF8}, /* VTS = 504 */
		{0x3814, 0x35}, {0x3815, 0x35}, /* 2x2 binned + subsampled (/4) */
		{0x3801, 0x10},               /* x start = 16 */
		{0x3806, 0x07}, {0x3807, 0x9F}, /* y end = 1951 */
		{0x3811, 0x00}, {0x3813, 0x00}, /* exact output window, no pad */
		{0x3820, 0x41}, {0x3821, 0x07}, /* vflip/vbin + mirror/hbin */
		{0x3018, 0x44},               /* MIPI 2-lane */
		{0x3034, 0x08},               /* 8-bit mode */
	};
	int bad = 0;
	for (uint32_t i = 0; i < sizeof(expect)/sizeof(expect[0]); i++) {
		uint8_t v = ov5647_read_reg(expect[i].reg);
		if (v != expect[i].val) {
			printf("ov5647: verify %04x=%02x (expect %02x)\n",
					expect[i].reg, v, expect[i].val);
			bad++;
		}
	}
	return bad == 0 ? 0 : -1;
}

int ov5647_set_mode(int mode) {
	uint8_t vc;
	int retry;

	if (mode != OV5647_MODE_640x480)
		return -1; /* only RAW8 VGA supported */

	for (retry = 0; retry < 3; retry++) {
		int failed = write_reg_table(ov5647_640x480_raw8);
		proc_usleep(10000);
		if (failed == 0 && verify_mode_regs() == 0)
			break;
		printf("ov5647: mode apply incomplete (failed=%d), retry %d\n",
				failed, retry);
		i2c_bus_recovery();
		proc_usleep(10000);
	}
	if (retry >= 3)
		return -1;

	/* force virtual channel 0 */
	vc = ov5647_read_reg(OV5647_REG_MIPI_CTRL14);
	ov5647_write_reg_checked(OV5647_REG_MIPI_CTRL14, (uint8_t)(vc & ~(3u << 6)));

	/* gate the MIPI clock lane (LP-11) until stream_on */
	ov5647_stream_off();
	return 0;
}

/*----------------------------------------------------------------------------*/
int ov5647_stream_on(void) {
	/* MIPI_CTRL00: bus idle only -> clock lane running (continuous) */
	ov5647_write_reg_checked(OV5647_REG_MIPI_CTRL00, 0x04);
	ov5647_write_reg_checked(OV5647_REG_FRAME_OFF_NUM, 0x00);
	ov5647_write_reg_checked(OV5647_REG_PAD_OUT, 0x00);
	proc_usleep(50000); /* 50ms for first frame to stabilize */
	printf("ov5647: stream on, 4800=%02x\n",
			ov5647_read_reg(OV5647_REG_MIPI_CTRL00));
	return 0;
}

/*----------------------------------------------------------------------------*/
int ov5647_stream_off(void) {
	/* MIPI_CTRL00: clock lane gate + bus idle + clock lane disable */
	ov5647_write_reg_checked(OV5647_REG_MIPI_CTRL00, 0x25);
	ov5647_write_reg_checked(OV5647_REG_FRAME_OFF_NUM, 0x0F);
	ov5647_write_reg_checked(OV5647_REG_PAD_OUT, 0x01);
	return 0;
}

/*----------------------------------------------------------------------------*/
int ov5647_bytes_per_pixel(int mode) {
	(void)mode;
	return 1; /* RAW8: 1 byte per pixel */
}
