#include <string.h>

#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>

#include <arch/bcm283x/mailbox.h>
#include <arch/bcm283x/gpio.h>

enum {
	// The GPIO registers base address.
	GPIO_BASE_OFF = 0x00200000, 

	// The offsets for reach register.

	// Controls actuation of pull up/down to ALL GPIO pins.
	GPPUD = (GPIO_BASE_OFF + 0x94),

	// Controls actuation of pull up/down for specific GPIO pin.
	GPPUDCLK0 = (GPIO_BASE_OFF + 0x98),

	// The base address for UART.
	UART0_BASE_OFF = 0x00201000, 

	// The offsets for reach register for the UART.
	UART0_DR = (UART0_BASE_OFF + 0x00),
	UART0_RSRECR = (UART0_BASE_OFF + 0x04),
	UART0_FR = (UART0_BASE_OFF + 0x18),
	UART0_ILPR = (UART0_BASE_OFF + 0x20),
	UART0_IBRD = (UART0_BASE_OFF + 0x24),
	UART0_FBRD = (UART0_BASE_OFF + 0x28),
	UART0_LCRH = (UART0_BASE_OFF + 0x2C),
	UART0_CR = (UART0_BASE_OFF + 0x30),
	UART0_IFLS = (UART0_BASE_OFF + 0x34),
	UART0_IMSC = (UART0_BASE_OFF + 0x38),
	UART0_RIS = (UART0_BASE_OFF + 0x3C),
	UART0_MIS = (UART0_BASE_OFF + 0x40),
	UART0_ICR = (UART0_BASE_OFF + 0x44),
	UART0_DMACR = (UART0_BASE_OFF + 0x48),
	UART0_ITCR = (UART0_BASE_OFF + 0x80),
	UART0_ITIP = (UART0_BASE_OFF + 0x84),
	UART0_ITOP = (UART0_BASE_OFF + 0x88),
	UART0_TDR = (UART0_BASE_OFF + 0x8C),
};

#define BCM2835_MBOX_TAG_GET_CLOCK_RATE 0x00030002u
#define MAILBOX_VC_ALIAS_NONCACHED 0x40000000u
#define RPI_FIRMWARE_UART_CLK_ID 2u
#define PL011_DEFAULT_CLOCK_HZ 3000000u
#define PL011_BAUD_RATE 115200u

static uint32_t _pl011_uart_clock_hz = PL011_DEFAULT_CLOCK_HZ;
static uint32_t _pl011_uart_ibrd = 1u;
static uint32_t _pl011_uart_fbrd = 40u;

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

static inline void delay(int32_t n) {
	while(n > 0) n--;
}

static uint32_t bcm283x_pl011_uart_get_clock_rate(void) {
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
		bcm283x_mailbox_call(&msg);
		if (req->get_clock_rate.body.resp.rate_hz != 0) {
			clock_rate = req->get_clock_rate.body.resp.rate_hz;
		}
	}

	dma_free(0, req_vaddr);
	return clock_rate;
}

static void bcm283x_pl011_uart_set_baud(uint32_t clock_hz, uint32_t baud_rate) {
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
	put32(_mmio_base+UART0_IBRD, ibrd);
	put32(_mmio_base+UART0_FBRD, fbrd);
}

static void bcm283x_pl011_uart_prepare_gpio14_15(void) {
	// Disable pull up/down for all GPIO pins & delay for 150 cycles.
	put32(_mmio_base+GPPUD, 0x00000000);
	delay(150);

	// Disable pull up/down for pin 14,15 & delay for 150 cycles.
	put32(_mmio_base+GPPUDCLK0, (1 << 14) | (1 << 15));
	delay(150);

	// Write 0 to GPPUDCLK0 to make it take effect.
	put32(_mmio_base+GPPUDCLK0, 0x00000000);
}

static void bcm283x_pl011_uart_prepare_bt_gpio30_33(void) {
	bcm283x_gpio_init();
	bcm283x_gpio_config(30, GPIO_ALTF3); /* CTS0 */
	bcm283x_gpio_config(31, GPIO_ALTF3); /* RTS0 */
	bcm283x_gpio_config(32, GPIO_ALTF3); /* TXD0 */
	bcm283x_gpio_config(33, GPIO_ALTF3); /* RXD0 */
	bcm283x_gpio_pull(30, GPIO_PULL_UP);
	bcm283x_gpio_pull(31, GPIO_PULL_NONE);
	bcm283x_gpio_pull(32, GPIO_PULL_NONE);
	bcm283x_gpio_pull(33, GPIO_PULL_UP);
}

static int32_t bcm283x_pl011_uart_init_common(void) {
	uint32_t uart_clock_hz;

	// Disable UART0.
	put32(_mmio_base+UART0_CR, 0x00000000);

	// Clear pending interrupts.
	put32(_mmio_base+UART0_ICR, 0x7FF);

	uart_clock_hz = bcm283x_pl011_uart_get_clock_rate();
	bcm283x_pl011_uart_set_baud(uart_clock_hz, PL011_BAUD_RATE);

	// Enable FIFO & 8 bit data transmissio (1 stop bit, no parity).
	put32(_mmio_base+UART0_LCRH, (1 << 4) | (1 << 5) | (1 << 6));

	// Mask all interrupts.
	/*put32(_mmio_base+UART0_IMSC,
			(1 << 1) | (1 << 4) | (1 << 5) | 
			(1 << 6) | (1 << 7) |
			(1 << 8) | (1 << 9) | (1 << 10));
			*/

	// Enable UART0, receive & transfer part of UART.
	put32(_mmio_base+UART0_CR, (1 << 0) | (1 << 8) | (1 << 9));
	return 0;
}

int32_t bcm283x_pl011_uart_init(void) {
	// External PL011 console uses GPIO14/15 and needs the pin pull state reset.
	bcm283x_pl011_uart_prepare_gpio14_15();
	return bcm283x_pl011_uart_init_common();
}

int32_t bcm283x_pl011_uart_init_no_gpio(void) {
	// Internal PL011 users, such as onboard Bluetooth on Pi 3, must not touch GPIO14/15.
	return bcm283x_pl011_uart_init_common();
}

int32_t bcm283x_pl011_uart_init_bt(void) {
	bcm283x_pl011_uart_prepare_bt_gpio30_33();
	put32(_mmio_base+UART0_CR, 0x00000000);
	put32(_mmio_base+UART0_ICR, 0x7FF);
	bcm283x_pl011_uart_set_baud(bcm283x_pl011_uart_get_clock_rate(), PL011_BAUD_RATE);
	put32(_mmio_base+UART0_LCRH, (1 << 4) | (1 << 5) | (1 << 6));
	put32(_mmio_base+UART0_CR, (1 << 0) | (1 << 8) | (1 << 9) | (1 << 14) | (1 << 15));
	return 0;
}

uint32_t bcm283x_pl011_uart_clock_hz(void) {
	return _pl011_uart_clock_hz;
}

uint32_t bcm283x_pl011_uart_ibrd(void) {
	return _pl011_uart_ibrd;
}

uint32_t bcm283x_pl011_uart_fbrd(void) {
	return _pl011_uart_fbrd;
}

static inline int32_t bcm283x_pl011_uart_ready_to_send(void) {
	if(get32(_mmio_base+UART0_FR) & (1 << 5))
		return -1;
	return 0;
}

static inline int32_t bcm283x_pl011_uart_ready_to_recv(void) {
	if(get32(_mmio_base+UART0_FR) & (1 << 4)) 
		return -1;
	return 0;
}

int32_t bcm283x_pl011_uart_send(uint8_t c) {
	// Wait for UART to become ready to transmit.
	while(bcm283x_pl011_uart_ready_to_send() != 0) {
		usleep(1000);
	}
	put32(_mmio_base+UART0_DR, c);
	return 0;
}

int32_t bcm283x_pl011_uart_recv(uint32_t timeout_ms) {
	uint32_t ms = 0;
	while(bcm283x_pl011_uart_ready_to_recv() != 0) {
		usleep(1000);
		ms++;
		if(ms > timeout_ms)
			return -1;
	}
	return get32(_mmio_base+UART0_DR);
}

int32_t bcm283x_pl011_uart_write(const void* data, uint32_t size) {
  int32_t i;
  for(i=0; i<(int32_t)size; i++) {
    char c = ((char*)data)[i];
    bcm283x_pl011_uart_send(c);
  }
  return i;
}
