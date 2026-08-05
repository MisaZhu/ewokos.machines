#ifndef BCM2712_PL011_UART_H
#define BCM2712_PL011_UART_H

#include <stdint.h>

/*
 * PL011 UART driver for BCM2712 (Raspberry Pi 5).
 *
 * The primary console UART is a PL011 at MMIO offset 0x01001000.
 * On Pi 5 the firmware (config.txt: enable_uart=1) configures GPIO14/15
 * for UART0 TX/RX, so no GPIO pinmux is needed.
 */

int32_t bcm2712_pl011_uart_init(void);
int32_t bcm2712_pl011_uart_write(const void* data, uint32_t size);
uint32_t bcm2712_pl011_uart_clock_hz(void);
uint32_t bcm2712_pl011_uart_ibrd(void);
uint32_t bcm2712_pl011_uart_fbrd(void);

#endif
