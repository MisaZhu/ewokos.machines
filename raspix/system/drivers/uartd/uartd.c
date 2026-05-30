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

#define MINI_UART_BASE_OFF 0x00215040
#define MINI_UART_IO_REG (_mmio_base + MINI_UART_BASE_OFF + 0x00)
#define MINI_UART_LSR_REG (_mmio_base + MINI_UART_BASE_OFF + 0x14)
#define MINI_UART_RXFIFO_AVAIL 0x01

#define PL011_UART0_BASE_OFF 0x00201000
#define PL011_UART0_DR (_mmio_base + PL011_UART0_BASE_OFF + 0x00)
#define PL011_UART0_FR (_mmio_base + PL011_UART0_BASE_OFF + 0x18)
#define PL011_UART_RXFIFO_EMPTY (1 << 4)

static charbuf_t *_RxBuf;
static bool _mini_uart;
static bool _no_return;
static uint32_t _idle_sleep_us;

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

	/*int i;
	for(i = 0; i < size; i++){
		char ch = ((char*)buf)[i];
		if(ch == '\r')
			ch = '\n';

		while(true){
			if(charbuf_push(&_TxBuf, ch, false) == 0){
				break;
			} 
			proc_usleep(100);
		};
	}
	return size;
	*/
	if(_mini_uart)
		return bcm283x_mini_uart_write(buf, size);
	else
		return bcm283x_pl011_uart_write(buf, size);
}

static inline bool uart_can_recv(void) {
	if(_mini_uart)
		return (get32(MINI_UART_LSR_REG) & MINI_UART_RXFIFO_AVAIL) != 0;
	return (get32(PL011_UART0_FR) & PL011_UART_RXFIFO_EMPTY) == 0;
}

static inline char uart_recv_byte(void) {
	if(_mini_uart)
		return (char)(get32(MINI_UART_IO_REG) & 0xFF);
	return (char)(get32(PL011_UART0_DR) & 0xFF);
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
	int rx = 0;

	if(!uart_can_recv()) {
		proc_usleep(_idle_sleep_us);
		if(_idle_sleep_us < 50000)
			_idle_sleep_us <<= 1;
		return 0;
	}

	ipc_disable();
	while(uart_can_recv()) {
		char c = uart_recv_byte();
		if(c == '\r' && _no_return)
			continue;

		charbuf_push(_RxBuf, c, true);
		rx++;
	}
	ipc_enable();

	if(rx > 0) {
		_idle_sleep_us = 1000;
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	}

	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1]: "/dev/tty0";
	_mmio_base = mmio_map();
	_mini_uart = true;
	_no_return = false;
	_idle_sleep_us = 1000;

	if(argc > 2 && strcmp(argv[2], "nr") == 0)
		_no_return = true;

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));

	sys_info_t sysinfo;
	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
	if(strcmp(sysinfo.machine, "raspberry-pi1") == 0 ||
			strcmp(sysinfo.machine, "raspberry-pi2b") == 0)  {
		strcpy(dev.name, "pl011_uart");
		_mini_uart = false;
		bcm283x_pl011_uart_init();
	}
	else {
		strcpy(dev.name, "mini_uart");
		bcm283x_mini_uart_init();
	}

	_RxBuf = charbuf_new(0);

	dev.read = uart_read;
	dev.write = uart_write;
	dev.loop_step = loop;
	dev.check_poll_events = uart_check_poll_events;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);

	charbuf_free(_RxBuf);
	return 0;
}
