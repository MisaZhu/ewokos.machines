#include <dev/uart.h>
#include "arch.h"

#define COM1_PORT 0x3F8

static int uart_tx_ready(void) {
	return (inb(COM1_PORT + 5) & 0x20) != 0;
}

static int uart_rx_ready(void) {
	return (inb(COM1_PORT + 5) & 0x01) != 0;
}

int32_t uart_dev_init(uint32_t baud) {
	uint16_t divisor;
	if (baud == 0) {
		baud = 115200;
	}
	divisor = (uint16_t)(115200 / baud);
	outb(COM1_PORT + 1, 0x00);
	outb(COM1_PORT + 3, 0x80);
	outb(COM1_PORT + 0, divisor & 0xFF);
	outb(COM1_PORT + 1, divisor >> 8);
	outb(COM1_PORT + 3, 0x03);
	outb(COM1_PORT + 2, 0xC7);
	outb(COM1_PORT + 4, 0x0B);
	return 0;
}

static void uart_putc(char c) {
	while (!uart_tx_ready()) {
	}
	outb(COM1_PORT, (uint8_t)c);
}

int32_t uart_write(const void* data, uint32_t size) {
	const char* s = (const char*)data;
	for (uint32_t i = 0; i < size; ++i) {
		if (s[i] == '\n') {
			uart_putc('\r');
		}
		uart_putc(s[i]);
	}
	return (int32_t)size;
}

int32_t uart_getc(void) {
	if (!uart_rx_ready()) {
		return -1;
	}
	return (int32_t)inb(COM1_PORT);
}
