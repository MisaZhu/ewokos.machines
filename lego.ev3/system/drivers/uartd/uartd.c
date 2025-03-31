#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/charbuf.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>
#include <ewoksys/interrupt.h>
#include <ewoksys/interrupt.h>

#include <arch/ev3/uart.h>
#include "fifo.h"

//#define UART_BASE		(0x01c42000)
#define UART_BASE       (0x01D0C000)

static fifo_t *_RxBuf;
static fifo_t *_TxBuf;

static int uart_read(int fd, int from_pid, fsinfo_t* node, 
		void* buf, int size, int offset, void* p) {
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)size;
	(void)offset;
	(void)p;

	int i;
    for(i = 0; i < size; i++){
		if(fifo_is_empty(_RxBuf))
			break;
		((char*)buf)[i] = fifo_pop(_RxBuf);
    }
    return (i==0)?VFS_ERR_RETRY:i;
}

static int uart_write(int fd, int from_pid, fsinfo_t* node,
		const void* buf, int size, int offset, void* p) {
	(void)fd;
	(void)node;
	(void)from_pid;
	(void)offset;
	(void)p;

	int i;
	char ch;
	for(i = 0; i < size; i++){
		fifo_push(_TxBuf,  ((char*)buf)[i]);
	}
	ev3_uart_enable_irq(UART_BASE, EV3_IRQ_TX, EV3_IRQ_ENABLE);
	return i;
}
//
//static int loop(void* p){
//	int rx = 0, tx = 0;
//	char ch;
//
//	ipc_disable();
//	if(ev3_uart_can_write(UART_BASE)){
//		for(int i = 0; i < 16; i++){
//			 if(fifo_pop(_TxBuf, &ch) != 0)
//				 break;
//			 ev3_uart_putc(UART_BASE, ch);
//			 tx++;
//		}
//	}
//	ipc_enable();
//	proc_usleep(10);
//	return 0;
//}
//
static void interrupt_handle(uint32_t interrupt, uint32_t p) {
	(void)interrupt;
	(void)p;
	char ch;
	int rx = 0;

	int irq = ev3_uart_get_irq(UART_BASE) >> 1;
	while(ev3_uart_can_read(UART_BASE)){
		fifo_push_unsafe(_RxBuf, ev3_uart_getc(UART_BASE));
		rx++;
	}

	if(ev3_uart_can_write(UART_BASE)){
		for(int i = 0 ; i < 16; i++){
			if(fifo_is_empty(_TxBuf)){
				ev3_uart_enable_irq(UART_BASE, EV3_IRQ_TX, EV3_IRQ_DISABLE);
				break;
			}
			ev3_uart_putc(UART_BASE, fifo_pop_unsafe(_TxBuf));
		}
	}

	if(rx){
		proc_wakeup(RW_BLOCK_EVT);
	}
}


int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1]: "/dev/tty1";
	_mmio_base = mmio_map();
	_TxBuf = fifo_new(4096);
	_RxBuf = fifo_new(128);

	ev3_uart_init(UART_BASE, 115200);

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "uart");
	dev.read = uart_read;
	dev.write = uart_write;
	//dev.loop_step = loop;

	static interrupt_handler_t handler;
	handler.data = 0;
	handler.handler = interrupt_handle;
	sys_interrupt_setup(53, &handler);
	ev3_uart_enable_irq(UART_BASE, EV3_IRQ_RX, EV3_IRQ_ENABLE);

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);

	fifo_free(_RxBuf);
	fifo_free(_TxBuf);

	return 0;
}
