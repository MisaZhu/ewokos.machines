#ifndef __RK_SPI_H__
#define __RK_SPI_H__

#include <stdint.h>

int rk_spi_xfer(uint8_t *tx, uint8_t *rx, int len);
int rk_spi_write(uint8_t *buf, int len);
int rk_spi_init(void);

#endif
