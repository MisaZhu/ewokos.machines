#ifndef BCM2712_PL011_UART_H
#define BCM2712_PL011_UART_H

#include <stdint.h>

/*
 * PL011 UART driver for BCM2712 (Raspberry Pi 5).
 *
 * The primary console UART is a PL011 at MMIO offset 0x01001000.
 * On Pi 5 the firmware (config.txt: enable_uart=1) configures GPIO14/15
 * for UART0 TX/RX and programs the baud divisors (921600 on the debug
 * UART, 115200 on GPIO14/15). This driver uses the UART exactly as the
 * firmware left it - it never reprograms the line, matching the kernel
 * console policy in machines/raspi5/kernel/bsp/uart.c.
 */

int32_t bcm2712_pl011_uart_init(void);
int32_t bcm2712_pl011_uart_write(const void* data, uint32_t size);
uint32_t bcm2712_pl011_uart_ibrd(void);
uint32_t bcm2712_pl011_uart_fbrd(void);

#endif
