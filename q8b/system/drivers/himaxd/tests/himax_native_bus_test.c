/* Bridge test: the protocol layer drives himax_reg_read through the native
 * bus; a fake full-duplex transport records the exact SPI frame and echoes
 * the payload, proving the frame path (protocol -> bus -> transfer -> bus ->
 * protocol) without hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "himax_protocol.h"
#include "himax_native_bus.h"

#define FAKE_LOG 32
typedef struct {
    uint8_t last_tx[HIMAX_XFER_MAX];
    size_t last_len;
    uint8_t log_tx[FAKE_LOG][HIMAX_XFER_MAX];
    size_t log_len[FAKE_LOG];
    unsigned log_n;
    unsigned xfers;
    unsigned fail;
} fake_ctx_t;

static int fake_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, size_t len) {
    fake_ctx_t* f = (fake_ctx_t*)ctx;
    f->xfers++;
    if (f->fail) {
        f->fail--;
        return -1;
    }
    if (len > HIMAX_XFER_MAX || !tx)
        return -1;
    memcpy(f->last_tx, tx, len);
    f->last_len = len;
    if (f->log_n < FAKE_LOG) {
        memcpy(f->log_tx[f->log_n], tx, len);
        f->log_len[f->log_n] = len;
        f->log_n++;
    }
    /* Reads (rx != NULL) echo a recognizable byte in the payload slots;
     * writes carry rx == NULL and only need recording. */
    if (rx != NULL) {
        memset(rx, 0, len);
        if (tx[0] == HIMAX_SPI_FUNCTION_READ)
            for (size_t i = HIMAX_BUS_R_HLEN; i < len; i++)
                rx[i] = (uint8_t)(0xa0 + i);
    }
    return 0;
}

int main(void) {
    fake_ctx_t fake;
    memset(&fake, 0, sizeof(fake));
    himax_native_bus_t back;
    memset(&back, 0, sizeof(back));
    back.mode = 1;
    back.ctx = &fake;
    back.xfer = fake_xfer;
    himax_bus_t bus = himax_native_bus_attach(&back);
    assert(bus.xfer != NULL);

    himax_dev_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.bus = bus;
    dev.reset = NULL;
    dev.delay_us = NULL;

    uint8_t rbuf[4];
    /* A register read of 4 bytes -> frame 3+4 = 7 bytes, exactly echoed. */
    fake.xfers = 0;
    assert(himax_reg_read(&dev, 0x1234, rbuf, 4) == 0);
    /* A 4-byte reg read drives: burst(2) + addr(1) + dir(1) + read(1) +
     * burst restore(2) = 7 SPI frames; the read frame is 3+4 bytes. */
    assert(fake.xfers == 7u && fake.log_n == 7u);
    /* Transaction 5 is the data read frame: 3(prefix)+4(payload)=7 bytes. */
    assert(fake.log_len[4] == 7u);
    assert(fake.log_tx[4][0] == HIMAX_SPI_FUNCTION_READ);
    /* dummy byte sits between cmd and payload; its value is not part of
     * the framing contract, so only the structure is checked. */
    assert(rbuf[0] == 0xa3 && rbuf[1] == 0xa4 &&
           rbuf[2] == 0xa5 && rbuf[3] == 0xa6); /* payload echo */

    /* Write 2 bytes: the data frame is 2(prefix)+4(addr)+2(payload)=8 bytes
     * and its payload sits at the end. */
    {
        const uint8_t wv[] = { 0x5a, 0xa5 };
        fake.log_n = 0;
        assert(himax_reg_write(&dev, 0x5678, wv, 2) == 0);
        /* burst(2) + data write(8) + burst restore(2); data frame at log[2]. */
        assert(fake.log_n == 5u && fake.log_len[2] == 8u);
        assert(fake.log_tx[2][0] == HIMAX_SPI_FUNCTION_WRITE);
        assert(fake.log_tx[2][6] == 0x5a && fake.log_tx[2][7] == 0xa5);
    }

    /* A failing transport surfaces as a bus error (retried then failure). */
    fake.fail = 99;                              /* fail all retries */
    assert(himax_reg_read(&dev, 0x0001, rbuf, 1) != 0);
    assert(dev.bus_errors > 0);
    fake.fail = 0;

    /* Unwired bus must be refused before touching any transport. */
    himax_native_bus_t none;
    memset(&none, 0, sizeof(none));
    none.xfer = fake_xfer;
    none.ctx = &fake;
    himax_bus_t nbus = himax_native_bus_attach(&none);
    assert(nbus.xfer == NULL || nbus.xfer(&none, (const uint8_t*)"", NULL, 1) != 0);
    (void)nbus;

    printf("himax native bus bridge tests: ok\n");
    return 0;
}