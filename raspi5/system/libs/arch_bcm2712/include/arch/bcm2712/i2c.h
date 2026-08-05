#ifndef BCM2712_I2C_H
#define BCM2712_I2C_H

#include <stdint.h>

/*
 * RP1 I2C driver stub for BCM2712.
 *
 * rp1.dtsi has seven controllers, i2c@70000 .. i2c@88000 at a 0x4000 stride
 * (i2c0-i2c6); the 40 pin header i2c1 is i2c@74000. Offsets from _mmio_base
 * are PI5_RP1_WIN_OFF + 0x70000 + bus * 0x4000.
 *
 * Not yet implemented.
 */

int bcm2712_i2c_init(int bus);
int bcm2712_i2c_read(int bus, uint8_t addr, uint8_t *buf, int len);
int bcm2712_i2c_write(int bus, uint8_t addr, const uint8_t *buf, int len);

#endif
