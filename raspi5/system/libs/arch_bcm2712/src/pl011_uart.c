#include <string.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <arch/bcm2712/mailbox.h>

/*
 * Pi 5 PL011 UART at MMIO offset 0x01001000.
 * Firmware sets up GPIO14/15, so we only configure baud rate.
 */
enum {
	UART0_BASE_OFF = 0x01001000,

	UART0_DR     = (UART0_BASE_OFF + 0x00),
	UART0_FR     = (UART0_BASE_OFF + 0x18),
	UART0_IBRD   = (UART0_BASE_OFF + 0x24),
	UART0_FBRD   = (UART0_BASE_OFF + 0x28),
	UART0_LCRH   = (UART0_BASE_OFF + 0x2C),
	UART0_CR     = (UART0_BASE_OFF + 0x30),
	UART0_ICR    = (UART0_BASE_OFF + 0x44),
};

#define BCM2835_MBOX_TAG_GET_CLOCK_RATE 0x00030002u
#define MAILBOX_VC_ALIAS_NONCACHED 0x40000000u
#define RPI_FIRMWARE_UART_CLK_ID 2u
#define PL011_DEFAULT_CLOCK_HZ 48000000u
#define PL011_BAUD_RATE 115200u

static uint32_t _pl011_uart_clock_hz = PL011_DEFAULT_CLOCK_HZ;
static uint32_t _pl011_uart_ibrd = 26u;   /* 48MHz / (16 * 115200) = 26 */
static uint32_t _pl011_uart_fbrd = 2u;    /* remainder → 2 */

typedef struct {
	uint32_t buf_size;
	uint32_t code;
} bcm2835_mbox_hdr_t;

typedef struct {
	uint32_t tag;
	uint32_t val_buf_size;
	uint32_t val_len;
} bcm2835_mbox_tag_hdr_t;

typedef struct {
	bcm2835_mbox_tag_hdr_t tag_hdr;
	union {
		struct {
			uint32_t clock_id;
		} req;
		struct {
			uint32_t clock_id;
			uint32_t rate_hz;
		} resp;
	} body;
} bcm2835_mbox_tag_get_clock_rate_t;

typedef struct {
	bcm2835_mbox_hdr_t hdr;
	bcm2835_mbox_tag_get_clock_rate_t get_clock_rate;
	uint32_t end_tag;
} msg_get_clock_rate_t;

static uint32_t bcm2712_pl011_uart_get_clock_rate(void) {
	ewokos_addr_t req_vaddr;
	msg_get_clock_rate_t* req;
	mail_message_t msg;
	uint32_t clock_rate = PL011_DEFAULT_CLOCK_HZ;
	uint32_t mailbox_data;

	req_vaddr = dma_alloc(0, sizeof(msg_get_clock_rate_t));
	if (req_vaddr == 0) {
		return clock_rate;
	}

	req = (msg_get_clock_rate_t*)(uintptr_t)req_vaddr;
	memset(req, 0, sizeof(*req));
	req->hdr.buf_size = sizeof(*req);
	req->get_clock_rate.tag_hdr.tag = BCM2835_MBOX_TAG_GET_CLOCK_RATE;
	req->get_clock_rate.tag_hdr.val_buf_size = sizeof(req->get_clock_rate.body);
	req->get_clock_rate.tag_hdr.val_len = sizeof(req->get_clock_rate.body.req);
	req->get_clock_rate.body.req.clock_id = RPI_FIRMWARE_UART_CLK_ID;

	mailbox_data = ((uint32_t)dma_phy_addr(0, req_vaddr) + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
	if (mailbox_data != 0) {
		msg.data = mailbox_data;
		msg.channel = PROPERTY_CHANNEL;
		bcm2712_mailbox_call(&msg);
		if (req->get_clock_rate.body.resp.rate_hz != 0) {
			clock_rate = req->get_clock_rate.body.resp.rate_hz;
		}
	}

	dma_free(0, req_vaddr);
	return clock_rate;
}

static void bcm2712_pl011_uart_set_baud(uint32_t clock_hz, uint32_t baud_rate) {
	uint32_t baud_div = 16u * baud_rate;
	uint32_t ibrd;
	uint32_t rem;
	uint32_t fbrd;

	if (clock_hz == 0 || baud_div == 0) {
		clock_hz = PL011_DEFAULT_CLOCK_HZ;
		baud_div = 16u * PL011_BAUD_RATE;
	}

	ibrd = clock_hz / baud_div;
	rem = clock_hz % baud_div;
	fbrd = ((rem * 64u) + (baud_div / 2u)) / baud_div;
	if (fbrd >= 64u) {
		ibrd += fbrd / 64u;
		fbrd %= 64u;
	}

	_pl011_uart_clock_hz = clock_hz;
	_pl011_uart_ibrd = ibrd;
	_pl011_uart_fbrd = fbrd;
	put32(_mmio_base + UART0_IBRD, ibrd);
	put32(_mmio_base + UART0_FBRD, fbrd);
}

int32_t bcm2712_pl011_uart_init(void) {
	uint32_t uart_clock_hz;

	/* Disable UART0 */
	put32(_mmio_base + UART0_CR, 0x00000000);

	/* Clear pending interrupts */
	put32(_mmio_base + UART0_ICR, 0x7FF);

	uart_clock_hz = bcm2712_pl011_uart_get_clock_rate();
	bcm2712_pl011_uart_set_baud(uart_clock_hz, PL011_BAUD_RATE);

	/* Enable FIFO & 8 bit data transmission (1 stop bit, no parity) */
	put32(_mmio_base + UART0_LCRH, (1 << 4) | (1 << 5) | (1 << 6));

	/* Enable UART0, receive & transmit */
	put32(_mmio_base + UART0_CR, (1 << 0) | (1 << 8) | (1 << 9));
	return 0;
}

uint32_t bcm2712_pl011_uart_clock_hz(void) {
	return _pl011_uart_clock_hz;
}

uint32_t bcm2712_pl011_uart_ibrd(void) {
	return _pl011_uart_ibrd;
}

uint32_t bcm2712_pl011_uart_fbrd(void) {
	return _pl011_uart_fbrd;
}

static inline int32_t bcm2712_pl011_uart_ready_to_send(void) {
	if (get32(_mmio_base + UART0_FR) & (1 << 5))
		return -1;
	return 0;
}

int32_t bcm2712_pl011_uart_write(const void* data, uint32_t size) {
	int32_t i;
	for (i = 0; i < (int32_t)size; i++) {
		char c = ((char*)data)[i];

		/* Wait for TX FIFO to have space */
		while (bcm2712_pl011_uart_ready_to_send() != 0) {
			usleep(1000);
		}
		put32(_mmio_base + UART0_DR, c);
	}
	return i;
}
