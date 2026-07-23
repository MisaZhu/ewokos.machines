#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/charbuf.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>
#include <arch/bcm283x/mini_uart.h>
#include <arch/bcm283x/pl011_uart.h>
#include <arch/bcm283x/spi.h>

#include "sc16is750.h"

static charbuf_t *_RxBuf = NULL;
static 	SC16IS750_t spiuart;
static bool _no_return;

static int uart_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, 
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)offset;
	(void)node;
	(void)size;
	(void)p;

	int i;
	for(i = 0; i < size; i++){
	int res = charbuf_pop(_RxBuf, buf + i);
	if(res != 0)
		break;
	}
	return (i==0)?VFS_ERR_RETRY:i;
}

static int uart_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		const void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)node;
	(void)from_pid;
	(void)offset;
	(void)p;
	for(int i = 0; i < size; i++){
		uint8_t c = ((uint8_t*)(buf+offset))[i];
		if(SC16IS750_write(&spiuart, SC16IS750_CHANNEL_B, c) != 0)
			return (i == 0) ? -1 : i; //chip not responding, report written bytes
	}
	return size;
}

static uint32_t uart_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)p;

	if(!charbuf_is_empty(_RxBuf))
		return VFS_EVT_RD;
	return 0;
}

static int loop(vdevice_t* dev, void* p) {
	(void)dev;
	(void)p;

	int len = SC16IS750_available(&spiuart, SC16IS750_CHANNEL_B);
	if(len <= 0) {
		proc_usleep(10000);
		return 0;
	}

	int rx = 0;
	ipc_disable();
	for(int i = 0; i < len; i++){
		int r = SC16IS750_read(&spiuart, SC16IS750_CHANNEL_B);
		if(r < 0) //FIFO raced or chip not responding, never push garbage
			break;
		char c = (char)r;
		if(c == '\r' && _no_return)
			continue;
		charbuf_push(_RxBuf, c, true);
		rx++;
	}
	ipc_enable();

	/*Level-triggered readiness is reported via uart_check_poll_events; the
	  wakeup edge must be sent once, outside the ipc_disable() window (a
	  per-byte wakeup inside it can be lost, leaving a blocked reader asleep
	  forever even though data sits in the buffer).*/
	if(rx > 0)
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1]: "/dev/tty1";
	_mmio_base = mmio_map();

	if(argc > 2 && strcmp(argv[2], "nr") == 0)
		_no_return = true;

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "spi_uart");

	SC16IS750_init(&spiuart, SC16IS750_PROTOCOL_SPI, 18, SC16IS750_DUAL_CHANNEL);
	SC16IS750_begin(&spiuart, SC16IS750_DEFAULT_SPEED, SC16IS750_DEFAULT_SPEED, 14745600UL); //baudrate&frequency setting

	if (SC16IS750_ping(&spiuart)!=1) 
		return 0;

	_RxBuf = charbuf_new(0);

	dev.read = uart_read;
	dev.write = uart_write;
	dev.loop_step = loop;
	dev.check_poll_events = uart_check_poll_events;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);

	charbuf_free(_RxBuf);
	return 0;
}
