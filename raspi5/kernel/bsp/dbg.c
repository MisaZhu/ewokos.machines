#include <mm/mmu.h>
#include <stdint.h>
#include "hw_arch.h"

/*
 * Low level serial debug output (PL011 raw access).
 * Lives in the normal kernel text so it stays callable after the final
 * kernel VM is installed (low memory is not identity mapped there).
 *
 * The firmware may not have enabled the UART (needs enable_uart=1 in
 * config.txt), so never block forever: each character gets a bounded
 * wait and is dropped on timeout.
 */
#define UART_FR  (MMIO_BASE + PI5_UART0_OFF + 0x18)
#define UART_DR  (MMIO_BASE + PI5_UART0_OFF + 0x00)
#define UART_FR_TXFF (1 << 5)

void pi5_dbg_putc(char c) {
    volatile uint32_t timeout = 0x200000;
    while((get32(UART_FR) & UART_FR_TXFF) && --timeout);
    if(timeout == 0)
        return;
    put32(UART_DR, c);
}

void pi5_dbg_puts(const char* s) {
    while(*s) {
        if(*s == '\n')
            pi5_dbg_putc('\r');
        pi5_dbg_putc(*s);
        s++;
    }
}

void pi5_dbg_hex(uint64_t v) {
    char buf[19] = "0x";
    const char* hex = "0123456789abcdef";
    for(int i = 15; i >= 0; i--)
        buf[17 - i] = hex[(v >> (i * 4)) & 0xF];
    buf[18] = 0;
    pi5_dbg_puts(buf);
    pi5_dbg_puts("\n");
}
