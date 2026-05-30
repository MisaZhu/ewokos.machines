#include <ewoksys/mmio.h>
#include <arch/bcm283x/gpio.h>

#define AUX_OFFSET 0x00215000
#define UART_OFFSET 0x00215040

#define AUX_BASE (_mmio_base | AUX_OFFSET)
#define UART_BASE (_mmio_base | UART_OFFSET)

#define AUX_ENABLES (AUX_BASE+0x04)
#define UART_AUX_ENABLE 0x01

#define UART_IO_REG   (UART_BASE+0x00)
#define UART_IER_REG  (UART_BASE+0x04)
#define UART_IIR_REG  (UART_BASE+0x08)
#define UART_LCR_REG  (UART_BASE+0x0C)
#define UART_MCR_REG  (UART_BASE+0x10)
#define UART_LSR_REG  (UART_BASE+0x14)
#define UART_MSR_REG  (UART_BASE+0x18)
#define UART_SCRATCH  (UART_BASE+0x1C)
#define UART_CNTL_REG (UART_BASE+0x20)
#define UART_STAT_REG (UART_BASE+0x24)
#define UART_BAUD_REG (UART_BASE+0x28)

#define UART_BAUD_115200 270
#define UART_BAUD_9600 3254
#define UART_BAUD_DEFAULT UART_BAUD_115200

#define UART_TXD_GPIO 14
#define UART_RXD_GPIO 15

static inline uint32_t get_baud(uint32_t uart_baud) {
	if(uart_baud == 9600)
		return UART_BAUD_9600;
	return UART_BAUD_DEFAULT;
}

int32_t bcm283x_mini_uart_init(void) {
	unsigned int data = get32(AUX_ENABLES);
	/* enable uart */
	put32(AUX_ENABLES, data | UART_AUX_ENABLE);
	/* configure uart */
	put32(UART_LCR_REG, 0x03);
	put32(UART_MCR_REG, 0x00);
	put32(UART_IER_REG, 0x00);
	put32(UART_BAUD_REG, get_baud(115200));
	bcm283x_gpio_pull(UART_TXD_GPIO, GPIO_PULL_NONE);
	/* RX idle must stay high; pull-up prevents a floating line from streaming 0x00 */
	bcm283x_gpio_pull(UART_RXD_GPIO, GPIO_PULL_UP);
	bcm283x_gpio_config(UART_TXD_GPIO, GPIO_ALTF5);
	bcm283x_gpio_config(UART_RXD_GPIO, GPIO_ALTF5);
	/* clear pending RX/TX state before enabling the port */
	put32(UART_IIR_REG, 0xC6);
	put32(UART_CNTL_REG, 0x03);
	return 0;
}

#define UART_TXFIFO_EMPTY 0x20
#define UART_RXFIFO_AVAIL 0x01

static inline int32_t bcm283x_mini_uart_ready_to_recv(void) {
	if((get32(UART_LSR_REG)&UART_RXFIFO_AVAIL) == 0)
		return -1;
	return 0;
}

static inline int32_t bcm283x_mini_uart_ready_to_send(void) {
	if((get32(UART_LSR_REG)&UART_TXFIFO_EMPTY) == 0)
		return -1;
	return 0;
}

int32_t bcm283x_mini_uart_recv(void) {
	while(bcm283x_mini_uart_ready_to_recv() != 0) {
		usleep(1000);
	}
	return get32(UART_IO_REG) & 0xFF;
}

int32_t bcm283x_mini_uart_send(uint8_t data) {
	while(bcm283x_mini_uart_ready_to_send() != 0) {
		usleep(1000);
	}
	put32(UART_IO_REG, data);
	return 0;
}

int32_t bcm283x_mini_uart_write(const void* data, uint32_t size) {
  int32_t i;
  for(i=0; i<(int32_t)size; i++) {
    char c = ((char*)data)[i];
    bcm283x_mini_uart_send(c);
  }
  return i;
}
