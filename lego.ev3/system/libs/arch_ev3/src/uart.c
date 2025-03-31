#include <stdio.h>
#include <stdint.h>
#include <ewoksys/mmio.h>


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
#define UART_LSR_DR     (0x01)

#define REG32(x) (*(volatile uint32_t*)(_mmio_base + base + (x)))

uint32_t ev3_uart_get_irq(uint32_t base){
	return REG32(UART_IIR);
}

void ev3_uart_enable_irq(uint32_t base, int dir, int en){
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

void ev3_uart_init(uint32_t base, int baudrate){
	uint16_t div = (int)(9375000.0f/baudrate + 0.5f);

	REG32(UART_PWREMU) = 0x1 | 0x1 << 13 | 0x1 << 14;
	REG32(UART_DLL) = div & 0xFF;
	REG32(UART_DLH) = (div >> 8) & 0xFF;

	REG32(UART_LCR) = 0x3; // 8n1
	
	REG32(UART_FCR) = 0x1;
}

int ev3_uart_can_write(uint32_t base){
	return ((REG32(UART_LSR)) & UART_LSR_THRE);
}

int ev3_uart_can_read(uint32_t base){
	return REG32(UART_LSR) & UART_LSR_DR;
}

void ev3_uart_putc(uint32_t base, char ch){
    REG32(UART_TX) = ch;
}

char ev3_uart_getc(uint32_t base){
	return  REG32(UART_TX);
}
