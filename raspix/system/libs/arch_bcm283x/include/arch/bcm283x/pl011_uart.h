#ifndef PL011_UART_H
#define PL011_UART_H

#include <stdint.h>

int32_t bcm283x_pl011_uart_init(void);
int32_t bcm283x_pl011_uart_init_no_gpio(void);
int32_t bcm283x_pl011_uart_init_bt(void);
uint32_t bcm283x_pl011_uart_clock_hz(void);
uint32_t bcm283x_pl011_uart_ibrd(void);
uint32_t bcm283x_pl011_uart_fbrd(void);
int32_t bcm283x_pl011_uart_write(const void* data, uint32_t size);
int32_t bcm283x_pl011_uart_send(const uint8_t c);
int32_t bcm283x_pl011_uart_recv(uint32_t timeout_ms);

#endif
