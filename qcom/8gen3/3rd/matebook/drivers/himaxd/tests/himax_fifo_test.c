/* End-to-end host test of the Himax read path through the FIFO transport:
 * himax_reg_read -> native bus (himax_native_bus) -> fifo backend
 * (himax_fifo) -> mock q8b-style transfer -> payload echo.  No hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "himax_protocol.h"
#include "himax_native_bus.h"
#include "himax_fifo.h"

#define HIST_MAX 16
typedef struct {
    uint8_t hist_buf[HIST_MAX][HIMAX_XFER_MAX];
    size_t hist_len[HIST_MAX];
    unsigned hn, xfers;
    unsigned fail;
    int echo;
} mock_t;

static int mock_transfer(void* ctx, const void* tx, void* rx, size_t len) {
    mock_t* m = (mock_t*)ctx;
    m->xfers++;
    if (m->fail) {
        m->fail--;
        return -1;
    }
    if (!tx || len > HIMAX_XFER_MAX)
        return -1;
    if (m->hn < HIST_MAX) {
        memcpy(m->hist_buf[m->hn], tx, len);
        m->hist_len[m->hn] = len;
        m->hn++;
    }
    if (m->echo && rx) {
        const uint8_t* t = (const uint8_t*)tx;
        uint8_t* r = (uint8_t*)rx;
        memset(r, 0, len);
        if (t[0] == HIMAX_SPI_FUNCTION_READ)
            for (size_t i = HIMAX_BUS_R_HLEN; i < len; i++)
                r[i] = (uint8_t)(0x40 + i);
    }
    return 0;
}

int main(void) {
    mock_t mock;
    memset(&mock, 0, sizeof(mock));
    mock.echo = 1;

    himax_fifo_t fifo;
    memset(&fifo, 0, sizeof(fifo));
    fifo.transfer = mock_transfer;
    fifo.spictx = &mock;
    fifo.ready = 1;
    himax_bus_t fbus = himax_fifo_attach(&fifo);   /* fifo dispatch */

    /* native bus on top of the fifo backend */
    himax_native_bus_t back;
    memset(&back, 0, sizeof(back));
    back.mode = 1;
    back.ctx = fbus.ctx;
    back.xfer = fbus.xfer;
    himax_bus_t bus = himax_native_bus_attach(&back);

    himax_dev_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.bus = bus;
    dev.reset = NULL;
    dev.delay_us = NULL;

    /* 4-byte register read: 7 fifo frames; the data read is transfer #5. */
    uint8_t rbuf[4];
    mock.hn = 0;
    assert(himax_reg_read(&dev, 0x100, rbuf, 4) == 0);
    assert(mock.hn == 7u);
    assert(mock.hist_len[4] == 7u);
    assert(mock.hist_buf[4][0] == HIMAX_SPI_FUNCTION_READ);
    assert(rbuf[0] == 0x43 && rbuf[1] == 0x44 && rbuf[2] == 0x45 &&
           rbuf[3] == 0x46);

    /* transport failure -> bus error surfaces and is retried */
    mock.hn = 0;
    mock.fail = 99;
    assert(himax_reg_read(&dev, 0x200, rbuf, 1) != 0);
    assert(dev.bus_errors > 0);
    mock.fail = 0;

    /* backend guards */
    uint8_t t[64];
    memset(t, 0, sizeof(t));
    assert(fbus.xfer(&fifo, t, t, 0) == -1);          /* len 0 */
    assert(fbus.xfer(&fifo, t, t, HIMAX_BUS_MAX_XFER + 1) == -1); /* too large */
    assert(strcmp(fifo.stage, "fifo-too-large") == 0);
    {
        himax_fifo_t closed;
        memset(&closed, 0, sizeof(closed));
        closed.transfer = mock_transfer;
        himax_bus_t cbus = himax_fifo_attach(&closed);
        assert(cbus.xfer(&closed, t, t, 4) == -1);    /* not open */
        assert(strcmp(closed.stage, "fifo-not-open") == 0);
    }
    printf("himax fifo path tests: ok\n");
    return 0;
}