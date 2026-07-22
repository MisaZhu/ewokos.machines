#include <dev/uart.h>
#include <mm/mmu.h>


#define UART_BASE		(0xa0000)
#define UART_LSR        (0x14)
#define UART_TX         (0x0)

#define UART_LSR_THRE	(0x20)

#define REG32(x) (*(volatile uint32_t*)(MMIO_BASE + UART_BASE + (x)))

int32_t uart_dev_init(uint32_t baud) {
	return 0;
}

int32_t uart_write(const void* data, uint32_t size) {
	for(int i = 0; i <  (int)size; i++){
	char ch = ((char*)data)[i];
		while ((REG32(UART_LSR) & UART_LSR_THRE) == 0);	
			REG32(UART_TX) = ch;
	}
	return 0;
}

