#include "himax_gpio.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <sched.h>
#include <stdlib.h>

#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>

#define TLMM_PHY                  0x0f100000ULL
/* Dedicated TLMM slot; this process maps nothing else of the low MMIO window. */
#define TLMM_VA                   0x58200000ULL
#define TLMM_SIZE                 0x00300000U

#define GPIO_MISO                 154U
#define GPIO_MOSI                 155U
#define GPIO_CLK                  156U
#define GPIO_CS                   157U
#define GPIO_IRQ                  175U
#define GPIO_RESET                99U
#define GPIO_MODE                 174U

#define GPIO_STRIDE               0x1000U
#define GPIO_CTL(g)               ((uint32_t)(g) * GPIO_STRIDE)
#define GPIO_IO(g)                (GPIO_CTL(g) + 4U)
#define GPIO_IN                   (1U << 0)
#define GPIO_OUT                  (1U << 1)
#define GPIO_OE                   (1U << 9)
#define GPIO_PULL_MASK            0x3U
#define GPIO_FUNC_MASK            (0xfU << 2)
#define GPIO_DRV_MASK             (0x7U << 6)
#define GPIO_DRV_2MA              0U

/*
 * Yield policy for the bit-bang transport.  HIMAX_SPI_YIELD selects it:
 *   0 (default) pure busy-wait: fast, and the only variant measured with
 *     <1% torn frames on the device.
 *   1 sched_yield() at byte boundaries: hands the scheduler a slice with
 *     only a context-switch gap on the wires.
 *   2 usleep(500) at byte boundaries: yields for half a millisecond.
 *
 * Long gaps mid-frame tear the panel's raw record stream (measured), so 2
 * drops frame quality; 1 is the safe yield form.
 */
#define HIMAX_YIELD_EVERY_BYTES   512U
#define HIMAX_YIELD_US            500U
static unsigned _yield_mode;

static volatile uint8_t* _tlmm;
static himax_dev_t _dev;
/* Set when CS is driven as a plain GPIO (GENI transport): the CS pad must not
 * be handed to the serial engine in that case. */

static unsigned _half_period_us;

/*
 * TLMM is Device-nGnRnE: accesses to it are not reordered and a store does
 * not complete early, so the bit-bang loop needs no explicit dmb between a
 * pad write and the following MISO read.  A compiler barrier keeps the
 * programmed order; the measured cost of the hardware barrier was most of
 * the frame transfer budget.
 */
static inline uint32_t rd32(unsigned offset) {
    uint32_t value = *(volatile uint32_t*)(_tlmm + offset);
    __asm__ volatile("" ::: "memory");
    return value;
}

static inline void wr32(unsigned offset, uint32_t value) {
    __asm__ volatile("" ::: "memory");
    *(volatile uint32_t*)(_tlmm + offset) = value;
}

/*
 * We own these four pads, so their output latch is shadowed here instead of
 * being read back before every edge.  The input bit of GPIO_IO is read-only
 * and may be written as zero.
 */
static uint32_t _latch[192];

static void pin_level(unsigned gpio, bool high) {
    uint32_t io;

    if (high)
        _latch[gpio] |= GPIO_OUT;
    else
        _latch[gpio] &= ~GPIO_OUT;
    io = _latch[gpio];
    wr32(GPIO_IO(gpio), io);
}

static bool pin_input(unsigned gpio) {
    return (rd32(GPIO_IO(gpio)) & GPIO_IN) != 0;
}

/*
 * Prepare one pad.  The latch is written before output enable so an output
 * never glitches when the firmware left a different level behind.
 */
static void pin_config(unsigned gpio, unsigned function, unsigned pull,
        bool output, bool level) {
    uint32_t ctl = rd32(GPIO_CTL(gpio));

    ctl &= ~(GPIO_FUNC_MASK | GPIO_DRV_MASK | GPIO_PULL_MASK | GPIO_OE);
    ctl |= (function & 0xfU) << 2;
    ctl |= GPIO_DRV_2MA << 6;
    ctl |= pull & GPIO_PULL_MASK;
    if (output) {
        _latch[gpio] = level ? GPIO_OUT : 0;
        wr32(GPIO_IO(gpio), _latch[gpio]);
        ctl |= GPIO_OE;
    }
    wr32(GPIO_CTL(gpio), ctl);
    (void)rd32(GPIO_CTL(gpio));
}

/*
 * SPI mode 3: the clock idles high, the master changes MOSI while the clock
 * is high, the IC shifts on the leading (falling) edge and the master samples
 * MISO on the trailing (rising) edge.  One transaction = one CS-low window,
 * matching one spi_transfer message in the Linux driver.
 */
static int gpio_spi_xfer(void* ctx, const uint8_t* tx, uint8_t* rx,
        size_t len) {
    (void)ctx;
    pin_level(GPIO_CS, false);
    for (size_t i = 0; i < len; i++) {
        uint8_t out = tx ? tx[i] : 0xffU;
        uint8_t in = 0;
        for (unsigned bit = 0; bit < 8U; bit++) {
            bool high = (out & (uint8_t)(0x80U >> bit)) != 0;
            /* A store to an unchanged latch value is still a full uncached
             * MMIO write; grid data changes on roughly half the bits, so
             * skipping the equal ones is a measurable win in a loop that
             * runs 41000 times per frame. */
            if (high != ((_latch[GPIO_MOSI] & GPIO_OUT) != 0))
                pin_level(GPIO_MOSI, high);
            if (_half_period_us)
                usleep(_half_period_us);
            pin_level(GPIO_CLK, false);
            if (_half_period_us)
                usleep(_half_period_us);
            pin_level(GPIO_CLK, true);
            if (rx && pin_input(GPIO_MISO))
                in |= (uint8_t)(0x80U >> bit);
            if (_half_period_us)
                usleep(_half_period_us);
        }
        if (rx)
            rx[i] = in;
        if (_yield_mode && ((i + 1U) & (HIMAX_YIELD_EVERY_BYTES - 1U)) == 0U) {
            if (_yield_mode == 1U)
                sched_yield();
            else
                usleep(HIMAX_YIELD_US);
        }
    }
    pin_level(GPIO_CS, true);
    return 0;
}

static void hw_reset(void* ctx, bool assert) {
    (void)ctx;
    /* reset-gpios is active low: assert drives the pad low. */
    pin_level(GPIO_RESET, !assert);
}

static void hw_delay_us(void* ctx, unsigned usec) {
    (void)ctx;
    usleep(usec);
}

bool himax_gpio_open(void) {
    const char* mode = getenv("HIMAX_SPI_YIELD");

    _yield_mode = mode ? (unsigned)atoi(mode) : 0U;
    if (syscall3(SYS_MEM_MAP, (ewokos_addr_t)TLMM_VA,
            (ewokos_addr_t)TLMM_PHY, (ewokos_addr_t)TLMM_SIZE) != TLMM_VA) {
        slog("gaokun3 himax: TLMM mapping failed\n");
        return false;
    }
    _tlmm = (volatile uint8_t*)(uintptr_t)TLMM_VA;

    /* Select the SPI host interface before the IC reloads its firmware:
     * gpio174 low, 2 mA, no pull.  The vendor firmware leaves it high, which
     * silently keeps the IC on I2C-HID while SPI register access still works. */
    pin_config(GPIO_MODE, 0, 0, true, false);

    /* Bus idle: CS inactive high, clock high (mode 3), MOSI high. */
    pin_config(GPIO_CS, 0, 1, true, true);
    pin_config(GPIO_CLK, 0, 1, true, true);
    pin_config(GPIO_MOSI, 0, 1, true, true);
    pin_config(GPIO_MISO, 0, 1, false, false);
    pin_config(GPIO_IRQ, 0, 3, false, false);
    /* Leave reset released: the probe sequence drives its own reset pulse.
     * A diagnostic run must not reset a panel that a live daemon owns. */
    pin_config(GPIO_RESET, 0, 0, true, true);

    memset(&_dev, 0, sizeof(_dev));
    _dev.bus.xfer = gpio_spi_xfer;
    _dev.bus.ctx = NULL;
    _dev.reset = hw_reset;
    _dev.delay_us = hw_delay_us;
    _dev.hw_ctx = NULL;
    _dev.stage = "startup";
    return true;
}

himax_dev_t* himax_gpio_dev(void) {
    return &_dev;
}

/* Native-transport pin setup: host-interface select low, IRQ input and
 * reset released, but *not* the SPI pins (the GENI QUP0 SE6 controller owns
 * gpio154-157 as qup6 function 1 and would conflict with bit-bang pins
 * here).  The transport xfer must be swapped to the FIFO backend right
 * after; reset/delay callbacks are the same as the bit-bang daemon. */
bool himax_gpio_open_native_pins(void) {
    _yield_mode = 0U;
    if (syscall3(SYS_MEM_MAP, (ewokos_addr_t)TLMM_VA,
            (ewokos_addr_t)TLMM_PHY, (ewokos_addr_t)TLMM_SIZE) != TLMM_VA) {
        slog("gaokun3 himax: TLMM mapping failed\n");
        return false;
    }
    _tlmm = (volatile uint8_t*)(uintptr_t)TLMM_VA;
    pin_config(GPIO_MODE, 0, 0, true, false);   /* gpio174 low = SPI */
    pin_config(GPIO_IRQ, 0, 3, false, false);   /* gpio175 input */
    pin_config(GPIO_RESET, 0, 0, true, true);   /* reset released */
    /* All four pads are handed to the serial engine by q8b_spi_open(). */
    _dev.bus.xfer = NULL;                        /* native backend sets it */
    _dev.bus.ctx = NULL;
    _dev.reset = hw_reset;
    _dev.delay_us = hw_delay_us;
    _dev.hw_ctx = NULL;
    _dev.stage = "startup";
    return true;
}

void himax_gpio_set_xfer(int (*xfer)(void*, const uint8_t*, uint8_t*, size_t),
        void* ctx) {
    _dev.bus.xfer = xfer ? xfer : gpio_spi_xfer;
    _dev.bus.ctx = ctx;
}

bool himax_gpio_irq_asserted(void) {
    return !pin_input(GPIO_IRQ);
}

/* Index order is kept stable for the diagnostic output. */
static const struct {
    unsigned gpio;
    const char* name;
} _pads[] = {
    { GPIO_MISO,  "miso"  },
    { GPIO_MOSI,  "mosi"  },
    { GPIO_CLK,   "clk"   },
    { GPIO_CS,    "cs"    },
    { GPIO_IRQ,   "irq"   },
    { GPIO_RESET, "reset" },
    { GPIO_MODE,  "mode"  },
};

unsigned himax_gpio_pin_count(void) {
    return (unsigned)(sizeof(_pads) / sizeof(_pads[0]));
}

const char* himax_gpio_pin_name(unsigned index) {
    if (!_tlmm || index >= himax_gpio_pin_count())
        return NULL;
    return _pads[index].name;
}

unsigned himax_gpio_pin_number(unsigned index) {
    if (index >= himax_gpio_pin_count())
        return 0;
    return _pads[index].gpio;
}

bool himax_gpio_pad_read(unsigned index, himax_pad_t* out) {
    unsigned gpio;

    if (!_tlmm || !out || index >= himax_gpio_pin_count())
        return false;
    gpio = _pads[index].gpio;
    out->ctl = rd32(GPIO_CTL(gpio));
    out->io = rd32(GPIO_IO(gpio));
    return true;
}

bool himax_gpio_sample_input(unsigned gpio, unsigned pull, bool* level) {
    if (!_tlmm || !level)
        return false;
    pin_config(gpio, 0, pull, false, false);
    usleep(2000);
    *level = pin_input(gpio);
    return true;
}

void himax_gpio_set_mode(bool high) {
    if (_tlmm)
        pin_level(GPIO_MODE, high);
}

void himax_gpio_hold_reset(bool assert) {
    if (_tlmm)
        pin_level(GPIO_RESET, !assert);
}

bool himax_gpio_probe_device(void) {
    return himax_probe(&_dev) == 0;
}

void himax_gpio_set_half_period_us(unsigned half_us) {
    _half_period_us = half_us;
}

unsigned himax_gpio_half_period_us(void) {
    return _half_period_us;
}
