#ifndef BCM2712_SPI_H
#define BCM2712_SPI_H

#include <stdint.h>

/*
 * RP1 SPI master driver for BCM2712 (Raspberry Pi 5).
 *
 * The Pi 5 has no BCM283x SPI block; the buses are Synopsys DW_apb_ssi
 * instances inside the RP1 southbridge ("snps,dw-apb-ssi" in rp1.dtsi),
 * clocked from clk_sys (200 MHz):
 *
 *   spi0 @ 0x50000 .. spi7 @ 0x6c000 (0x4000 stride), spi8 @ 0x4c000.
 *
 * The 40-pin header spi0 sits on funcsel a0: SCLK GPIO11, MOSI GPIO10,
 * MISO GPIO9; init() muxes those. Chip select is NOT the controller's
 * native SS output: DW_apb_ssi drops SS whenever the TX FIFO underruns,
 * so (like the official device tree, which lists cs-gpios for spi0)
 * CE0/GPIO8 and CE1/GPIO7 are driven as plain RIO outputs by
 * bcm2712_spi_activate(). That also gives the activate()/deactivate()
 * bracket semantics the bsp drivers expect.
 *
 * Buses other than 0 are initialized but left to the caller to pinmux
 * and to handle CS.
 *
 * SCLK = 200MHz / div (div even, >= 2); mode 0, polled.
 */

#define SPI_SELECT_0 0x01
#define SPI_SELECT_1 0x02
#define SPI_SELECT_DEFAULT SPI_SELECT_0

int  bcm2712_spi_init(int bus);
int  bcm2712_spi_set_div(int bus, uint32_t div);
int  bcm2712_spi_transfer(int bus, const void *tx, void *rx, uint32_t len);
int  bcm2712_spi_write16(int bus, const uint16_t *tx, uint32_t count);
/* assert (enable=1) / release (enable=0) the CS selected by _select() */
int  bcm2712_spi_activate(int bus, uint32_t enable);
/* which: SPI_SELECT_0 / SPI_SELECT_1 */
int  bcm2712_spi_select(int bus, uint32_t which);

#endif
