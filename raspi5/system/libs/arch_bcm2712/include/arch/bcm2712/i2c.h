#ifndef BCM2712_I2C_H
#define BCM2712_I2C_H

#include <stdint.h>

/* RP1 DesignWare I2C controllers 0-3. Bus 1 is GPIO2/3 on the 40-pin header. */

#define BCM2712_I2C_INVALID  -1
#define BCM2712_I2C_NODEV    -2
#define BCM2712_I2C_TIMEOUT  -3
#define BCM2712_I2C_BUSY     -4
#define BCM2712_I2C_NACK     -5

int bcm2712_i2c_init(int bus);
int bcm2712_i2c_read(int bus, uint8_t addr, uint8_t *buf, int len);
int bcm2712_i2c_write(int bus, uint8_t addr, const uint8_t *buf, int len);
int bcm2712_i2c_write_read(int bus, uint8_t addr,
		const uint8_t *wbuf, int wlen, uint8_t *rbuf, int rlen);

#endif
