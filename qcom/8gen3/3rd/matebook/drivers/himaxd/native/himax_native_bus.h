#ifndef HIMAX_NATIVE_BUS_H
#define HIMAX_NATIVE_BUS_H
#include <stdint.h>
#include <stddef.h>
#include "himax_protocol.h"

/*
 * Bridge between the Himax protocol layer and a native SPI transport
 * (host tests only; not linked into himaxd yet).  The protocol layer only
 * sees himax_bus_t.xfer; this file provides the wiring and the FIFO backend
 * selection on top of the bsp SPI controller candidate.
 *
 * SPI selection for gaokun3: gpio174 low = SPI host interface, then the
 * QUP0 SE6 engine (0x998000) in mode 3.  The FIFO backend below calls a
 * transfer function (full duplex, same length on TX and RX) supplied by the
 * caller; the GPI-DMA backend is not wired yet (needs SMMU route for SID
 * 0x576 and kernel-pinned buffers), and an uninitialized/unwired backend is
 * refused with a clear stage.
 */
typedef struct {
    int (*xfer)(void* ctx, const uint8_t* tx, uint8_t* rx, size_t len);
    void* ctx;
    const char* stage;
    uint8_t mode;                 /* 1 = FIFO wired, 0 = none */
} himax_native_bus_t;

/* Build the himax_bus_t view for himax_protocol using a full-duplex native
 * transport.  back->xfer is invoked for every SPI frame (same len in/out).
 * Returns a himax_bus_t that can be plugged into himax_dev_t.bus. */
himax_bus_t himax_native_bus_attach(himax_native_bus_t* back);

/* Convenience full-duplex adapter for a bsp-style transfer callback. */
int himax_bus_fifo_callback(void* ctx, const uint8_t* tx, uint8_t* rx,
                            size_t len);

#endif