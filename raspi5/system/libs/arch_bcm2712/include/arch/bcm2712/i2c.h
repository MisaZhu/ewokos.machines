#ifndef BCM2712_I2C_H
#define BCM2712_I2C_H

#include <stdint.h>

/*
 * RP1 I2C driver stub for BCM2712.
 *
 * RP1 provides 4 I2C controllers (I2C0-3) at RP1-internal base 0x40044000+.
 * Host offset from _mmio_base: 0x08044000+
 *
 * Not yet implemented.
 */

int bcm2712_i2c_init(int bus);
int bcm2712_i2c_read(int bus, uint8_t addr, uint8_t *buf, int len);
int bcm2712_i2c_write(int bus, uint8_t addr, const uint8_t *buf, int len);

#endif
