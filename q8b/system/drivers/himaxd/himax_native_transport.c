/* Native-SPI transport backend for himaxd: QUP0 SE6 (bus 60, mode 3) FIFO
 * master instead of the GPIO bit-bang master.  Everything else (probe, raw
 * frame read, hx_algo solving, /dev/touch0 publishing) is unchanged.
 *
 * Pin split: himax_gpio_open_native_pins() takes gpio174 low (SPI host
 * select), keeps gpio175 as the IRQ input and gpio99 reset released.  All four
 * touch pads (gpio154-157) belong to the GENI engine as qup6 function 1; the
 * engine also drives chip select, which is the configuration that reads the
 * IC reliably (see docs/spi-geni-bringup-findings.md).
 *
 * Selected with: himaxd --native [/dev/touch0] [frame-ms]
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <bsp/spi.h>

#include "himax_gpio.h"
#include "himax_protocol.h"
#include "native/himax_fifo.h"

/* Record read clock.  See himax_native_transport_open(). */
#define Q8B_HIMAX_SPI_HZ 9600000u

static q8b_spi_t _spi;
static himax_fifo_t _fifo;
static bool _opened;

static void tlog(const char* fmt, ...) {
    FILE* f = fopen("/tmp/transport.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

himax_dev_t* himax_native_transport_open(void);

/*
 * Re-initialise the SPI transport.
 *
 * The GENI engine is set up once, at daemon start.  If the daemon happens to
 * start before the engine is ready to answer (a cold boot races the QUP clock
 * and ICC votes), every subsequent transfer fails - spi-read-failed, bus errors
 * climbing - and nothing in the retry path ever touched the engine again, so
 * the driver stayed dead for the whole session.  Re-running open() gives the
 * engine a clean setup; the pinmux and the IC are left alone.
 */
himax_dev_t* himax_native_transport_reopen(void) {
    _opened = false;
    memset(&_spi, 0, sizeof(_spi));
    memset(&_fifo, 0, sizeof(_fifo));
    return himax_native_transport_open();
}

himax_dev_t* himax_native_transport_open(void) {
    if (_opened)
        return himax_gpio_dev();
    if (!himax_gpio_open_native_pins()) {
        tlog("pins-failed\n");
        return NULL;
    }
    tlog("pins-ok\n");
    /*
     * 9.6 MHz is the fast end of what the QUP0 SE6 divider produces from the
     * 19.2 MHz source and well under the IC's 12.5 MHz rating; the record
     * read dominates everything else, and this is what makes it cheap:
     * 5132 bytes took ~42.9 ms at 960 kHz and ~5.4 ms here, with the same
     * records accepted.
     */
    if (q8b_spi_open(&_spi, 60, Q8B_HIMAX_SPI_HZ,
                Q8B_SPI_CPHA | Q8B_SPI_CPOL) != 0) {
        tlog("spi-open-failed stage=%s\n", _spi.stage ? _spi.stage : "?");
        slog("gaokun3 himax: native QUP0 SE6 open failed: %s\n",
                _spi.stage ? _spi.stage : "?");
        return NULL;
    }
    tlog("spi-open-ok hz=%u se=%08x\n", _spi.speed_hz, (unsigned)_spi.se_pa);
    memset(&_fifo, 0, sizeof(_fifo));
    _fifo.transfer = himax_fifo_q8b_spi;
    _fifo.spictx = &_spi;
    _fifo.ready = 1;
    himax_bus_t bus = himax_fifo_attach(&_fifo);
    himax_gpio_set_xfer(bus.xfer, bus.ctx);
    _opened = true;
    tlog("transport-ready\n");
    slog("gaokun3 himax: native SPI transport hz=%u se=%08x\n",
            _spi.speed_hz, (unsigned)_spi.se_pa);
    return himax_gpio_dev();
}