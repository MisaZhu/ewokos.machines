#ifndef HIMAX_FIFO_H
#define HIMAX_FIFO_H
#include <stddef.h>
#include <stdint.h>
#include "himax_protocol.h"

/*
 * FIFO transport backend for the native himax bus (host tests only; not
 * linked into himaxd).  Wires himax_bus_t.xfer to a full-duplex SPI
 * transfer function (normally q8b_spi_transfer on a bus-60 QUP0 SE6
 * handle), with bounds checks.  Full-duplex SPI frames larger than
 * Q8B_SPI_MAX_XFER cannot be split because CS must stay asserted for the
 * whole Himax access; the backend refuses them and reports "too-large".
 */
#define HIMAX_BUS_MAX_XFER 6144U

typedef struct {
    int (*transfer)(void* spictx, const void* tx, void* rx, size_t len);
    void* spictx;
    const char* stage;
    uint8_t ready;              /* set once the SPI handle is open */
} himax_fifo_t;

/* Build the himax bus view backed by himax_fifo_t. */
himax_bus_t himax_fifo_attach(himax_fifo_t* fifo);

/* Default transfer hook for a q8b_spi_t* spictx (raw pointer). */
int himax_fifo_q8b_spi(void* spictx, const void* tx, void* rx, size_t len);

#endif