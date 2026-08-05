#include <arch/bcm2712/spi.h>

/* Stub: RP1 SPI driver not yet implemented. */
int bcm2712_spi_init(int bus)     { (void)bus; return -1; }
int bcm2712_spi_set_div(int bus, uint32_t div) { (void)bus; (void)div; return -1; }
int bcm2712_spi_transfer(int bus, const void *tx, void *rx, uint32_t len)
	{ (void)bus; (void)tx; (void)rx; (void)len; return -1; }
int bcm2712_spi_activate(int bus, uint32_t cs) { (void)bus; (void)cs; return -1; }
int bcm2712_spi_select(int bus, uint32_t cs)   { (void)bus; (void)cs; return -1; }
