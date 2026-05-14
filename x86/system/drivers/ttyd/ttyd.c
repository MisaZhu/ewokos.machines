#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/charbuf.h>
#include <bsp/x86_pio.h>

#define COM1_PORT 0x3F8

static inline int uart_tx_ready(void) {
	return (x86_inb(COM1_PORT + 5) & 0x20) != 0;
}

static inline int uart_rx_ready(void) {
	return (x86_inb(COM1_PORT + 5) & 0x01) != 0;
}

static void uart_init(void) {
	uint16_t divisor = 1; /* 115200 baud */
	x86_outb(COM1_PORT + 1, 0x00);
	x86_outb(COM1_PORT + 3, 0x80);
	x86_outb(COM1_PORT + 0, divisor & 0xFF);
	x86_outb(COM1_PORT + 1, divisor >> 8);
	x86_outb(COM1_PORT + 3, 0x03);
	x86_outb(COM1_PORT + 2, 0xC7);
	x86_outb(COM1_PORT + 4, 0x0B);
}

static void uart_putc(char c) {
	while (!uart_tx_ready()) {
	}
	x86_outb(COM1_PORT, (uint8_t)c);
}

static charbuf_t* _buffer = NULL;
static bool _wakeup = false;

static void tty_poll_input(void) {
	for (;;) {
		if (!uart_rx_ready()) {
			break;
		}
		int ch = (int)x86_inb(COM1_PORT);
		if (ch == '\r') {
			ch = '\n';
		}
		charbuf_push(_buffer, (char)ch, true);
		_wakeup = true;
	}
}

static int tty_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)info;
	(void)offset;
	(void)p;

	tty_poll_input();

	int i;
	char* out = (char*)buf;
	for (i = 0; i < size; i++) {
		if (charbuf_pop(_buffer, out + i) != 0) {
			break;
		}
	}
	return (i == 0) ? VFS_ERR_RETRY : i;
}

static uint32_t tty_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)info;
	(void)p;

	tty_poll_input();
	if (!charbuf_is_empty(_buffer)) {
		return VFS_EVT_RD;
	}
	return 0;
}

static int tty_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
		const void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)info;
	(void)offset;
	(void)p;

	const char* s = (const char*)buf;
	for (int i = 0; i < size; ++i) {
		if (s[i] == '\n') {
			uart_putc('\r');
		}
		uart_putc(s[i]);
	}
	return size;
}

static int tty_loop(vdevice_t* dev, void* p) {
	(void)p;

	tty_poll_input();
	if (_wakeup) {
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
		_wakeup = false;
	}
	usleep(3000);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/tty0";

	uart_init();
	_buffer = charbuf_new(0);
	if (_buffer == NULL) {
		return -1;
	}

	vdevice_t dev = {0};
	dev.name[0] = 't';
	dev.name[1] = 't';
	dev.name[2] = 'y';
	dev.name[3] = 0;
	dev.read = tty_read;
	dev.write = tty_write;
	dev.loop_step = tty_loop;
	dev.check_poll_events = tty_check_poll_events;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);
	charbuf_free(_buffer);
	return 0;
}
