#include <ewoksys/syscall.h>
#include <ewoksys/dma.h>
#include <arch/bcm2712/mailbox.h>

#include <string.h>

#define MAILBOX_TIMEOUT_LOOPS 0x2000000u
#define MAILBOX_STATUS_EMPTY (1u << 30)
#define MAILBOX_STATUS_FULL  (1u << 31)

#define readl(addr)  (*((volatile uint32_t *)(addr)))
#define writel(val, addr) (*((volatile uint32_t *)(addr)) = (uint32_t)(val))

ewokos_addr_t bcm2712_mailbox_init(void) {
	return mmio_map();
}

static inline uint32_t mailbox_read_data_raw(void) {
	return readl(MAILBOX_BASE + 0x00);
}

static inline uint32_t mailbox_read_status_raw(void) {
	return readl(MAILBOX_BASE + 0x18);
}

static inline void mailbox_write_data_raw(uint32_t value) {
	writel(value, MAILBOX_BASE + 0x20);
}

static inline uint32_t mailbox_pack_message(const mail_message_t* msg) {
	return ((msg->data & 0x0fffffffu) << 4) | (msg->channel & 0x0fu);
}

static inline void mailbox_unpack_message(mail_message_t* msg, uint32_t raw) {
	memset(msg, 0, sizeof(*msg));
	msg->channel = raw & 0x0fu;
	msg->data = raw >> 4;
}

static int mailbox_wait_nonempty(uint32_t timeout_loops) {
	while (timeout_loops-- > 0) {
		if ((mailbox_read_status_raw() & MAILBOX_STATUS_EMPTY) == 0) {
			return 0;
		}
	}
	return -1;
}

static int mailbox_wait_nonfull(uint32_t timeout_loops) {
	while (timeout_loops-- > 0) {
		if ((mailbox_read_status_raw() & MAILBOX_STATUS_FULL) == 0) {
			return 0;
		}
	}
	return -1;
}

void bcm2712_mailbox_read(mail_message_t *msg) {
	uint8_t channel = msg->channel;
	uint32_t raw;

	/* Make sure that the message is from the right channel */
	do {
		if (mailbox_wait_nonempty(MAILBOX_TIMEOUT_LOOPS) != 0) {
			return;
		}
		raw = mailbox_read_data_raw();
		mailbox_unpack_message(msg, raw);
	} while (msg->channel != channel);
}

void bcm2712_mailbox_send(mail_message_t* msg) {
	if (mailbox_wait_nonfull(MAILBOX_TIMEOUT_LOOPS) != 0) {
		return;
	}
	mailbox_write_data_raw(mailbox_pack_message(msg));
}

void bcm2712_mailbox_call(mail_message_t* msg) {
	bcm2712_mailbox_send(msg);
	bcm2712_mailbox_read(msg);
}

int bcm2712_mailbox_call_timeout(mail_message_t* msg, uint32_t timeout_loops) {
	uint32_t attempts = timeout_loops == 0 ? MAILBOX_TIMEOUT_LOOPS : timeout_loops;
	uint8_t channel = msg->channel;
	uint32_t raw_msg = mailbox_pack_message(msg);

	if (mailbox_wait_nonfull(attempts) != 0) {
		return -1;
	}
	mailbox_write_data_raw(raw_msg);

	for (uint32_t loops = attempts; loops > 0; --loops) {
		if (mailbox_read_status_raw() & MAILBOX_STATUS_EMPTY) {
			continue;
		}

		raw_msg = mailbox_read_data_raw();
		mailbox_unpack_message(msg, raw_msg);
		if (msg->channel == channel) {
			return 0;
		}
	}
	return -1;
}
