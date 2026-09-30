#include <stdio.h>
#include <stdint.h>
#include <ewoksys/mmio.h>

#include "../include/arch/ev3/uart.h"

#define UART_PWREMU		(0x30)

#define UART_TX         (0x0)
#define UART_RX         (0x0)
#define UART_IER		(0x4)
#define UART_IIR        (0x8)
#define UART_FCR        (0x8)
#define UART_LCR        (0xC)
#define UART_MCR        (0x10)
#define UART_LSR        (0x14)
#define UART_MSR        (0x18)
#define UART_SCR        (0x1C)
#define UART_DLL        (0x20)
#define UART_DLH        (0x24)
#define UART_REV1		(0x28)
#define UART_REV2		(0x2C)

#define UART_LSR_THRE   (0x20)
#define UART_LSR_TEMT   (0x40)
#define UART_LSR_DR     (0x01)

#define UART_FCR_FIFOEN (0x01)
#define UART_FCR_RXCLR  (0x02)
#define UART_FCR_TXCLR  (0x04)

/* UART functional clock is PLL0_SYSCLK2 (150 MHz) / 16 */
#define UART_CLK_DIV16  9375000u

static inline uint16_t baud_div(int baudrate) {
    if (baudrate <= 0) baudrate = 2400;
    return (uint16_t)((UART_CLK_DIV16 + (uint32_t)baudrate / 2) / (uint32_t)baudrate);
}

#define REG32(x) (*(volatile uint32_t*)(_mmio_base + base + (x)))

uint32_t ev3_uart_get_irq(ewokos_addr_t base){
    return REG32(UART_IIR);
}

void ev3_uart_enable_irq(ewokos_addr_t base, int dir, int en){
    uint32_t mask;

    if(dir)
        mask = 0x2;
    else
        mask = 0x1;
        
    if(en)
        REG32(UART_IER) |= mask;
    else
        REG32(UART_IER) &= ~mask;
}

void ev3_uart_init(ewokos_addr_t base, int baudrate){
    uint16_t div = baud_div(baudrate);

    REG32(UART_PWREMU) = 0x1 | 0x1 << 13 | 0x1 << 14;
    REG32(UART_DLL) = div & 0xFF;
    REG32(UART_DLH) = (div >> 8) & 0xFF;

    REG32(UART_LCR) = 0x3; // 8n1
    
    REG32(UART_FCR) = UART_FCR_FIFOEN | UART_FCR_RXCLR | UART_FCR_TXCLR;
}

void ev3_uart_set_baud(ewokos_addr_t base, int baudrate){
    uint16_t div = baud_div(baudrate);
    REG32(UART_DLL) = div & 0xFF;
    REG32(UART_DLH) = (div >> 8) & 0xFF;
    ev3_uart_flush_rx(base);
}

void ev3_uart_flush_rx(ewokos_addr_t base){
    REG32(UART_FCR) = UART_FCR_FIFOEN | UART_FCR_RXCLR;
}

int ev3_uart_tx_empty(ewokos_addr_t base){
    return ((REG32(UART_LSR)) & UART_LSR_TEMT);
}

int ev3_uart_can_write(ewokos_addr_t base){
    return ((REG32(UART_LSR)) & UART_LSR_THRE);
}

int ev3_uart_can_read(ewokos_addr_t base){
    return REG32(UART_LSR) & UART_LSR_DR;
}

void ev3_uart_putc(ewokos_addr_t base, char ch){
    REG32(UART_TX) = ch;
}

char ev3_uart_getc(ewokos_addr_t base){
    return  REG32(UART_TX);
}
