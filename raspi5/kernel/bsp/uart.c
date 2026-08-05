#include <dev/uart.h>
#include <kernel/hw_info.h>
#include <bcm2712/board.h>
#include <mm/mmu.h>
#include "hw_arch.h"

/*
 * PL011 (UART0) console driver for the Raspberry Pi 5.
 * The GPIO pinmux is left to the firmware (enable_uart=1 in config.txt),
 * only the UART itself is (re)initialized here.
 */

#define UART_REG(off) (MMIO_BASE + PI5_UART0_OFF + (off))

#define UART_DR    UART_REG(0x00)
#define UART_FR    UART_REG(0x18)
#define UART_IBRD  UART_REG(0x24)
#define UART_FBRD  UART_REG(0x28)
#define UART_LCRH  UART_REG(0x2C)
#define UART_CR    UART_REG(0x30)
#define UART_IFLS  UART_REG(0x34)
#define UART_IMSC  UART_REG(0x38)
#define UART_ICR   UART_REG(0x44)

#define UART_FR_TXFF (1 << 5)

#define UART_CLOCK_FALLBACK 48000000u

static uint32_t _uart_clk = 0;

static void uart_dbg(const char* s) {
	while(*s) {
		while(get32(UART_FR) & UART_FR_TXFF);
		if(*s == '\n') {
			put32(UART_DR, '\r');
			while(get32(UART_FR) & UART_FR_TXFF);
		}
		put32(UART_DR, *s);
		s++;
	}
}

static void uart_dbg_num(uint32_t v) {
	char buf[11];
	int i = 10;
	buf[10] = 0;
	if(v == 0) {
		uart_dbg("0");
		return;
	}
	while(v > 0 && i > 0) {
		buf[--i] = '0' + (v % 10);
		v /= 10;
	}
	uart_dbg(&buf[i]);
}

int32_t uart_dev_init(uint32_t baud) {
	if(baud == 0)
		baud = 115200;

	if(_uart_clk == 0) {
		uint32_t ibrd = get32(UART_IBRD);
		uint32_t fbrd = get32(UART_FBRD);
		if(ibrd != 0 || fbrd != 0) {
			uint64_t brd64 = ((uint64_t)ibrd << 6) | fbrd;
			_uart_clk = (uint32_t)(brd64 * 115200 / 4);
		}
		if(_uart_clk == 0)
			_uart_clk = UART_CLOCK_FALLBACK;
	}

	/*
	 * The firmware already configured the UART (enable_uart=1),
	 * and pi5_dbg_puts has been working since _boot_start.
	 * Skip reinitialization to avoid baud-rate mismatch.
	 */
#if 0
	/* Disable UART0 */
	put32(UART_CR, 0);

	/* Clear pending interrupts */
	put32(UART_ICR, 0x7FF);

	uint32_t div4 = _uart_clk * 4 / baud;
	put32(UART_IBRD, div4 >> 6);
	put32(UART_FBRD, div4 & 0x3F);

	/* Enable FIFO & 8 bit data transmission (1 stop bit, no parity) */
	put32(UART_LCRH, (1 << 4) | (1 << 5) | (1 << 6));

	/* Mask all interrupts */
	put32(UART_IMSC, (1 << 1) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) |
			(1 << 8) | (1 << 9) | (1 << 10));

	/* Enable UART0, receive & transmit */
	put32(UART_CR, (1 << 0) | (1 << 8) | (1 << 9));
#endif
	return 0;
}

static void uart_trans(char c) {
	/* Wait for UART to become ready to transmit */
	while (get32(UART_FR) & UART_FR_TXFF) {}
	put32(UART_DR, c);
}

int32_t uart_write(const void* data, uint32_t size) {
	int32_t i;
	for(i=0; i<(int32_t)size; i++) {
		char c = ((char*)data)[i];
		if(c == '\n')
			uart_trans('\r');
		uart_trans(c);
	}
	return i;
}
