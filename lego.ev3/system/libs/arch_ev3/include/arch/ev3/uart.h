#ifndef __EV3_UART_H__
#define __EV3_UART_H__

#include <stdint.h>
#include <ewoksys/ewokdef.h>

#define EV3_IRQ_RX		0
#define EV3_IRQ_TX		1

#define EV3_IRQ_DISABLE	0
#define EV3_IRQ_ENABLE	1

uint32_t ev3_uart_get_irq(ewokos_addr_t base);
void ev3_uart_enable_irq(ewokos_addr_t base, int dir, int en);
void ev3_uart_init(ewokos_addr_t base, int baudrate);
/* change divisor only (keeps IER/FCR) and reset RX FIFO */
void ev3_uart_set_baud(ewokos_addr_t base, int baudrate);
void ev3_uart_flush_rx(ewokos_addr_t base);
int ev3_uart_can_read(ewokos_addr_t base);
int ev3_uart_can_write(ewokos_addr_t base);
/* both THR and TSR empty: last bit has left the wire */
int ev3_uart_tx_empty(ewokos_addr_t base);
void ev3_uart_putc(ewokos_addr_t base, char ch);
char ev3_uart_getc(ewokos_addr_t base);

#endif
