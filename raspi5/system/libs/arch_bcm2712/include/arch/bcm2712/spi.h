#ifndef BCM2712_SPI_H
#define BCM2712_SPI_H

#include <stdint.h>

/*
 * RP1 SPI driver stub for BCM2712.
 *
 * RP1 provides 8 SPI controllers (SPI0-7) at RP1-internal base 0x40050000+.
 * Host offset from _mmio_base: 0x08050000+
 *
 * Not yet implemented.
 */

int  bcm2712_spi_init(int bus);
int  bcm2712_spi_set_div(int bus, uint32_t div);
int  bcm2712_spi_transfer(int bus, const void *tx, void *rx, uint32_t len);
int  bcm2712_spi_activate(int bus, uint32_t cs);
int  bcm2712_spi_select(int bus, uint32_t cs);

#endif
