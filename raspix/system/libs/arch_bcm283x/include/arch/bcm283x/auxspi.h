#ifndef AUX_SPI_ARCH_H
#define AUX_SPI_ARCH_H

#include <stdint.h>

int  bcm283x_auxspi_init(int channel);
void bcm283x_auxspi_set_clock(int clock);
void bcm283x_auxspi_transfer(uint8_t *tx, uint8_t *rx, uint32_t len);

#endif
