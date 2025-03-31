#ifndef __EV3_UART_H__
#define __EV3_UART_H__

#define EV3_IRQ_RX		0
#define EV3_IRQ_TX		1

#define EV3_IRQ_DISABLE	0
#define EV3_IRQ_ENABLE	1

void ev3_uart_enable_irq(uint32_t base, int dir, int en);
void ev3_uart_init(uint32_t base, int baudrate);
int ev3_uart_can_read(uint32_t base);
int ev3_uart_can_write(uint32_t base);
void ev3_uart_putc(uint32_t base, char ch);
char ev3_uart_getc(uint32_t base);

#endif
