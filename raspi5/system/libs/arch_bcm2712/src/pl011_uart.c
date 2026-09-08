#include <string.h>
#include <ewoksys/mmio.h>
#include <arch/bcm2712/mmio.h>

/*
 * Pi 5 PL011 UART at MMIO offset 0x01001000.
 *
 * The firmware (config.txt: enable_uart=1) fully programs the console
 * UART: pinmux, clock and the baud divisors - 921600 on the dedicated
 * debug UART, 115200 on GPIO14/15, depending on the setup. The kernel
 * console driver (machines/raspi5/kernel/bsp/uart.c) deliberately never
 * touches the divisors ("skip reinitialization to avoid baud-rate
 * mismatch") and this driver must follow the same policy.
 *
 * Reprogramming the rate from a mailbox clock query garbles output
 * whenever the queried clock or the firmware baud differs from the
 * 115200 assumption. The query itself was also broken: the request
 * buffer bus address was built as phys + 0x40000000 instead of
 * phys | 0x40000000, so on boards where dma_alloc buffers land above
 * 1GB (bit 30 already set) the firmware never saw the request at all
 * and the driver silently fell back to 48MHz divisors.
 */

enum {
    /* Register offsets are relative to the peripheral window base
       (_mmio_base); every UART register sits at +0x01001000. */
    UART0_BASE_OFF = 0x01001000,

    UART0_DR   = (UART0_BASE_OFF + 0x00),
    UART0_FR   = (UART0_BASE_OFF + 0x18),
    UART0_IBRD = (UART0_BASE_OFF + 0x24),
    UART0_FBRD = (UART0_BASE_OFF + 0x28),
};

#define UART_FR_TXFF (1 << 5)

int32_t bcm2712_pl011_uart_init(void) {
    /*
     * The UART is already running - the kernel console has been writing
     * to it since _boot_start. Preserve the firmware's configuration
     * (divisors, line control, enable state); touching any of it here
     * only risks switching the line to a rate the terminal is not on.
     */
    return 0;
}

/* Live divisor registers, for diagnostics only. */
uint32_t bcm2712_pl011_uart_ibrd(void) {
    return get32(_mmio_base + UART0_IBRD);
}

uint32_t bcm2712_pl011_uart_fbrd(void) {
    return get32(_mmio_base + UART0_FBRD);
}

static inline int32_t bcm2712_pl011_uart_ready_to_send(void) {
    if (get32(_mmio_base + UART0_FR) & UART_FR_TXFF)
        return -1;
    return 0;
}

int32_t bcm2712_pl011_uart_write(const void* data, uint32_t size) {
    int32_t i;
    for (i = 0; i < (int32_t)size; i++) {
        char c = ((char*)data)[i];

        /* Wait for TX FIFO to have space */
        while (bcm2712_pl011_uart_ready_to_send() != 0) {
            usleep(1000);
        }
        put32(_mmio_base + UART0_DR, c);
    }
    return i;
}
