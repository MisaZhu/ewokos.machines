#include <arch/bcm2712/i2c.h>

/* Stub: RP1 I2C driver not yet implemented. */
int bcm2712_i2c_init(int bus)   { (void)bus; return -1; }
int bcm2712_i2c_read(int bus, uint8_t addr, uint8_t *buf, int len)
	{ (void)bus; (void)addr; (void)buf; (void)len; return -1; }
int bcm2712_i2c_write(int bus, uint8_t addr, const uint8_t *buf, int len)
	{ (void)bus; (void)addr; (void)buf; (void)len; return -1; }
