#ifndef __EV3_UART_H__
#define __EV3_UART_H__

#include <stdint.h>
#include <ewoksys/ewokdef.h>

#define EV3_IRQ_RX		0
#define EV3_IRQ_TX		1

#define EV3_IRQ_DISABLE	0
#define EV3_IRQ_ENABLE	1

/*
 * PRU soft-UART sentinel.
 *
 * Input ports 3/4 have no 16550; they are driven by the PRU0 SUART firmware
 * (see pru_uart.h). To keep uart_sensor.c blind to the difference, port.c hands
 * back one of these sentinel "bases" instead of 0 for 3/4, and every
 * ev3_uart_*(base) below routes a sentinel to the PRU transport.
 *
 * Bit 30 is the discriminator: the three real 16550 bases (0x01C42000,
 * 0x01D0C000, ...) all sit below 0x40000000, so IS_PRU() is false for them and
 * the 16550 path is untouched. The low byte carries the EV3 input-port index
 * (EV3_IN_PORT_3 == 2, EV3_IN_PORT_4 == 3) that the PRU path decodes.
 */
#define EV3_UART_PRU_SENTINEL	0x40000000u
#define EV3_UART_PRU_BASE(port)	(EV3_UART_PRU_SENTINEL | (uint32_t)(port))
#define EV3_UART_IS_PRU(base)	(((uint32_t)(base) & EV3_UART_PRU_SENTINEL) != 0)
#define EV3_UART_PRU_PORT(base)	((int)((uint32_t)(base) & 0xFFu))

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
