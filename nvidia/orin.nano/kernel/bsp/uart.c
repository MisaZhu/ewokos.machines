#include <dev/uart.h>
#include <kernel/hw_info.h>
#include <mm/mmu.h>
#include <stdint.h>
#include "hw_arch.h"

/*
 * Kernel console on the Tegra234 AON UARTC (0x0c280000, 16550 mmio32).
 *
 * Firmware (SPE/TCU and UEFI) already enabled the UART clock, the pinmux
 * and 115200 8N1; we never touch the divisor or clocks.  TX only, polled
 * with a bounded wait, gated by the loader's LSR probe (orin_serial_on).
 */

extern void orin_dbg_write(const char* s, uint32_t size);

int32_t uart_dev_init(uint32_t baud) {
    (void)baud;
    return 0;
}

int32_t uart_write(const void* data, uint32_t size) {
    if (!orin_serial_on) {
        (void)data;
        return (int32_t)size;
    }
    orin_dbg_write((const char*)data, size);
    return (int32_t)size;
}