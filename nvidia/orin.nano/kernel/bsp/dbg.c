#include <mm/mmu.h>
#include <stdint.h>
#include "hw_arch.h"

/*
 * Raw 16550 debug output on the Tegra234 AON UARTC (0x0c280000), the
 * Orin Nano devkit debug console (J14 header, behind the firmware TCU).
 *
 * Firmware leaves UARTC clocked, pinmuxed and programmed for 115200 8N1
 * (hvisor uses the same earlycon: uart8250,mmio32,0xc280000).  This driver
 * only drives TX; it never reprograms the baud rate or clocks.  Registers
 * are 32-bit spaced (reg-shift=2).
 *
 * Runtime decision from the loader's LSR probe (bootinfo serial_flags
 * bit0, copied into orin_serial_on before the first kout).  Every wait is
 * bounded: a dead UART must never hang the kernel.
 */

#define UART_BASE   (MMIO_BASE + ORIN_UARTC_OFF)

#define UART_THR    0x00
#define UART_LSR    0x14
#define LSR_THRE    (1u << 5)
#define LSR_TEMT    (1u << 6)

static int dbg_serial_on(void) {
    return orin_serial_on;
}

void orin_dbg_putc(char c) {
    if (!dbg_serial_on())
        return;
    for (volatile uint32_t i = 0; i < 1000000; i++) {
        if (get32(UART_BASE + UART_LSR) & LSR_THRE)
            break;
    }
    put32(UART_BASE + UART_THR, (uint32_t)(uint8_t)c);
}

void orin_dbg_write(const char* s, uint32_t size) {
    if (!dbg_serial_on())
        return;
    for (uint32_t i = 0; i < size; i++) {
        if (s[i] == '\n')
            orin_dbg_putc('\r');
        orin_dbg_putc(s[i]);
    }
}

void orin_dbg_puts(const char* s) {
    if (!dbg_serial_on())
        return;
    while (*s) {
        if (*s == '\n')
            orin_dbg_putc('\r');
        orin_dbg_putc(*s++);
    }
}

void orin_dbg_hex(uint64_t v) {
    static const char h[] = "0123456789abcdef";
    orin_dbg_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        orin_dbg_putc(h[(v >> i) & 0xF]);
}

void orin_dbg_dec(uint64_t v) {
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    buf[i] = 0;
    if (v == 0)
        buf[--i] = '0';
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    orin_dbg_puts(&buf[i]);
}