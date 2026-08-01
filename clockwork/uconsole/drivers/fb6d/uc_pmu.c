#include "uc_pmu.h"
#include "uc_time.h"

#include <stdint.h>

#include <arch/bcm283x/gpio.h>
#include <arch/bcm283x/i2c.h>

/*
 * AXP223 register map, from Linux axp20x-regulator.c (AXP22X descs):
 *   ALDO1  vol 0x28 [4:0], en 0x10 bit6   ("audio-vdd")
 *   ALDO2  vol 0x29 [4:0], en 0x10 bit7   ("display-vcc"  <-- panel!)
 *   DLDO2  vol 0x16 [4:0], en 0x12 bit4
 *   DLDO3  vol 0x17 [4:0], en 0x12 bit5
 *   DLDO4  vol 0x18 [4:0], en 0x12 bit6
 * LDO range 700..3300mV in 100mV steps: 3300mV => sel 26 (0x1A).
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

static int _try_once(void) {
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
	if ((v & CTRL1_ALDO2_EN) == 0) {
		return -1;
	}
	v = i2c_getb(AXP_ADDR, AXP_PWR_OUT_CTRL2);
	if ((v & (CTRL2_DLDO2_EN | CTRL2_DLDO3_EN | CTRL2_DLDO4_EN)) !=
			(CTRL2_DLDO2_EN | CTRL2_DLDO3_EN | CTRL2_DLDO4_EN)) {
		return -1;
	}
	return 0;
}

int uc_pmu_display_power(void) {
	int attempt;

	bcm283x_gpio_init();
	/* Same bit-banged bus powerd uses: SDA=GPIO0, SCL=GPIO1. */
	i2c_init(0, 1);

	/*
	 * powerd polls the same bus every 300ms from another process; a
	 * collided transaction just reads back wrong, so retry a few
	 * times rather than trusting a single pass.
	 */
	for (attempt = 0; attempt < 5; attempt++) {
		if (_try_once() == 0) {
			/* Let the panel rails rise + panel logic settle. */
			uc_mdelay(20);
			return 0;
		}
		uc_mdelay(5);
	}
	return -1;
}
