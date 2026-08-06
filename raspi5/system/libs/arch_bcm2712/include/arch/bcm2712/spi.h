#ifndef BCM2712_SPI_H
#define BCM2712_SPI_H

#include <stdint.h>

/*
 * RP1 SPI driver stub for BCM2712.
 *
 * rp1.dtsi has spi@50000 .. spi@6c000 at a 0x4000 stride (spi0-spi7, plus
 * spi8 at 0x4c000). Offsets from _mmio_base are
 * PI5_RP1_WIN_OFF + 0x50000 + bus * 0x4000.
 *
 * Not yet implemented.
 */

int  bcm2712_spi_init(int bus);
int  bcm2712_spi_set_div(int bus, uint32_t div);
int  bcm2712_spi_transfer(int bus, const void *tx, void *rx, uint32_t len);
int  bcm2712_spi_activate(int bus, uint32_t cs);
int  bcm2712_spi_select(int bus, uint32_t cs);

#endif
