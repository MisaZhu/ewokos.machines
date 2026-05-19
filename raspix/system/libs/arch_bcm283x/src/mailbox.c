#include <ewoksys/syscall.h>
#include <arch/bcm283x/mailbox.h>

#include <string.h>

#define MAILBOX_TIMEOUT_LOOPS 0x2000000u
#define MAILBOX_STATUS_EMPTY (1u << 30)
#define MAILBOX_STATUS_FULL  (1u << 31)

#define readl(addr) (*((volatile uint32_t *)(addr)))
#define writel(val, addr) (*((volatile uint32_t *)(addr)) = (uint32_t)(val))

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

uint32_t bcm283x_mailbox_init(void) {
	return mmio_map();
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

void bcm283x_mailbox_read(mail_message_t *msg) {
	uint8_t channel = msg->channel;
	uint32_t raw;

	// Make sure that the message is from the right channel
	do {
		// Make sure there is mail to recieve
		if (mailbox_wait_nonempty(MAILBOX_TIMEOUT_LOOPS) != 0) {
			return;
		}

		// Get the message
		raw = mailbox_read_data_raw();
		mailbox_unpack_message(msg, raw);
	} while (msg->channel != channel);
}

void  bcm283x_mailbox_send(mail_message_t* msg) {
	// Make sure you can send mail
	if (mailbox_wait_nonfull(MAILBOX_TIMEOUT_LOOPS) != 0) {
		return;
	}

	// send the message
	mailbox_write_data_raw(mailbox_pack_message(msg));
}

void  bcm283x_mailbox_call(mail_message_t* msg) {
	bcm283x_mailbox_send(msg);
	bcm283x_mailbox_read(msg);
}

int bcm283x_mailbox_call_timeout(mail_message_t* msg, uint32_t timeout_loops) {
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
