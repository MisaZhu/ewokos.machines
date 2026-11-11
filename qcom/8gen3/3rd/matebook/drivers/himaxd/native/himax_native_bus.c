#include "himax_native_bus.h"
#include <string.h>

static int bus_dispatch(void* ctx, const uint8_t* tx, uint8_t* rx, size_t len) {
    himax_native_bus_t* back = (himax_native_bus_t*)ctx;
    if (!back || !back->xfer || !tx || len == 0 ||
            len > HIMAX_XFER_MAX) {
        if (back)
            back->stage = "native-bus-params";
        return -1;
    }
    if (!back->mode) {
        back->stage = "native-bus-unwired";   /* no FIFO/GPI backend yet */
        return -1;
    }
    if (back->xfer(back->ctx, tx, rx, len) != 0) {
        back->stage = "native-bus-xfer-failed";
        return -1;
    }
    back->stage = "native-bus-ok";
    return 0;
}

himax_bus_t himax_native_bus_attach(himax_native_bus_t* back) {
    himax_bus_t bus;
    memset(&bus, 0, sizeof(bus));
    bus.ctx = back;
    bus.xfer = bus_dispatch;
    return bus;
}

int himax_bus_fifo_callback(void* ctx, const uint8_t* tx, uint8_t* rx,
                            size_t len) {
    /* Placeholder adapter: the caller passes a struct holding a bsp SPI
     * handle and this would call q8b_spi_transfer(handle, tx, rx, len).
     * Kept as a plain function per the full-duplex contract; the bridge
     * test wires its own fake transfer to prove the frame path. */
    (void)ctx;
    (void)tx;
    (void)rx;
    (void)len;
    return -1;
}