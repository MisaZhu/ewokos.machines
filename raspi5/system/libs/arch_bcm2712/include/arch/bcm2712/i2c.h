#ifndef BCM2712_I2C_H
#define BCM2712_I2C_H

#include <stdint.h>

/*
 * RP1 I2C master driver for BCM2712 (Raspberry Pi 5).
 *
 * Unlike BCM283x (where i2c was bit-banged over GPIO), the Pi 5 routes all
 * I2C through the RP1 southbridge, which instantiates seven Synopsys
 * DW_apb_i2c controllers ("snps,designware-i2c" in rp1.dtsi):
 *
 *   i2c0 @ 0x70000 .. i2c6 @ 0x88000, 0x4000 stride,
 *   clocked from clk_sys (200 MHz).
 *
 * Offsets from _mmio_base are PI5_RP1_WIN_OFF + 0x70000 + bus * 0x4000,
 * so the caller's process needs the RP1 window mapped; bcm2712_i2c_init()
 * takes care of that itself.
 *
 * The 40-pin header pins carry i2c on funcsel a3:
 *   i2c0: GPIO0/1   i2c1: GPIO2/3 (the header default, has board pull-ups)
 *   i2c2: GPIO4/5   i2c3: GPIO6/7
 * init() muxes those pins for bus 0-3; bus 4-6 sit on camera/display
 * connector pins and are left to the caller to pinmux.
 *
 * All transfers are 7-bit address, polled, master only.
 */

#define BCM2712_I2C_BUS_HEADER  1    /* GPIO2/3 on the 40-pin header */

#define BCM2712_I2C_ERR_INVALID   -1
#define BCM2712_I2C_ERR_MAIN_MAP  -2
#define BCM2712_I2C_ERR_RP1_MAP   -3
#define BCM2712_I2C_ERR_DISABLE   -4
#define BCM2712_I2C_ERR_COMP_TYPE -5

int bcm2712_i2c_init(int bus);
/*
 * Same as bcm2712_i2c_init(), but muxes sda/scl explicitly (both pulled
 * up, funcsel a3) instead of the header default pair. Needed for bus 4-6
 * (display/camera connector control buses, e.g. bus 4 = GPIO40/41 =
 * i2c_csi_dsi) and whenever a bus's default pair is used by other logic.
 */
int bcm2712_i2c_init_pins(int bus, uint32_t sda, uint32_t scl);
/* hz <= 100000 selects standard mode, anything above selects fast mode (400k) */
int bcm2712_i2c_set_speed(int bus, uint32_t hz);

int bcm2712_i2c_write(int bus, uint8_t addr, const uint8_t *buf, int len);
int bcm2712_i2c_read(int bus, uint8_t addr, uint8_t *buf, int len);
/* write wlen bytes then repeated-start and read rlen bytes (register access) */
int bcm2712_i2c_write_read(int bus, uint8_t addr,
		const uint8_t *wbuf, int wlen, uint8_t *rbuf, int rlen);

/* single register helpers, same shape as the old bcm283x i2c_putb/i2c_getb */
int bcm2712_i2c_putb(int bus, uint8_t addr, uint8_t reg, uint8_t data);
int bcm2712_i2c_getb(int bus, uint8_t addr, uint8_t reg);

#endif
