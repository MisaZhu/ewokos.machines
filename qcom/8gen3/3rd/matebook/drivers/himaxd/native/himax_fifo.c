#include "himax_fifo.h"
#include <string.h>
#ifndef HIMAX_FIFO_HOST_TEST
#include <bsp/spi.h>
#endif

static int fifo_dispatch(void* ctx, const uint8_t* tx, uint8_t* rx,
                         size_t len) {
    himax_fifo_t* f = (himax_fifo_t*)ctx;
    if (!f || !tx || !len) {
        if (f) f->stage = "fifo-params";
        return -1;
    }
    if (!f->ready || !f->transfer) {
        f->stage = "fifo-not-open";
        return -1;
    }
    if (len > HIMAX_BUS_MAX_XFER) {
        /* CS must stay asserted for the whole Himax access; do not split. */
        f->stage = "fifo-too-large";
        return -1;
    }
    if (f->transfer(f->spictx, tx, rx, len) != 0) {
        f->stage = "fifo-xfer-failed";
        return -1;
    }
    f->stage = "fifo-ok";
    return 0;
}

himax_bus_t himax_fifo_attach(himax_fifo_t* fifo) {
    himax_bus_t bus;
    memset(&bus, 0, sizeof(bus));
    bus.ctx = fifo;
    bus.xfer = fifo_dispatch;
    return bus;
}

#ifdef HIMAX_FIFO_HOST_TEST
int himax_fifo_q8b_spi(void* spictx, const void* tx, void* rx, size_t len) {
    (void)spictx; (void)tx; (void)rx; (void)len;
    return -1;      /* host test supplies its own transport mock */
}
#else
int himax_fifo_q8b_spi(void* spictx, const void* tx, void* rx, size_t len) {
    q8b_spi_t* spi = (q8b_spi_t*)spictx;
    return q8b_spi_transfer(spi, tx, rx, len);
}
#endif