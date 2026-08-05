#include <ewoksys/syscall.h>
#include <ewoksys/dma.h>
#include <arch/bcm2712/mailbox.h>

#include <string.h>

#define MAILBOX_TIMEOUT_LOOPS 0x2000000u
#define MAILBOX_STATUS_EMPTY (1u << 30)
#define MAILBOX_STATUS_FULL  (1u << 31)

#define readl(addr)  (*((volatile uint32_t *)(addr)))
#define writel(val, addr) (*((volatile uint32_t *)(addr)) = (uint32_t)(val))

/*
 * The property buffer is Normal NonCacheable memory while the mailbox
 * registers are Device memory, and the two are not ordered against each other
 * by the architecture. Without a barrier the doorbell write can reach the VPU
 * before the request words have left the write buffers, so the firmware reads
 * a half written buffer; on the way back the reply words must not be read out
 * of a store that has not landed yet either.
 */
static inline void mailbox_barrier(void) {
	__asm__ __volatile__("dsb sy" ::: "memory");
}

ewokos_addr_t bcm2712_mailbox_init(void) {
	return mmio_map();
}

static inline uint32_t mailbox_read_data_raw(void) {
	return readl(MAILBOX_BASE + 0x00);
}

/*
 * There are two mailboxes. MBOX0 at +0x00 is the VPU->ARM direction and is the
 * only one the ARM may read; MBOX1 at +0x20 is ARM->VPU and is write only. Each
 * has its own status register, so the FULL flag that guards a write to MBOX1
 * lives at +0x38, not at MBOX0's +0x18. Reading MBOX0's status before writing
 * MBOX1 tests the wrong queue and lets a write land on a full outbound mailbox.
 */
static inline uint32_t mailbox_read_status_raw(void) {
	return readl(MAILBOX_BASE + 0x18);
}

static inline uint32_t mailbox_write_status_raw(void) {
	return readl(MAILBOX_BASE + 0x38);
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
		if ((mailbox_write_status_raw() & MAILBOX_STATUS_FULL) == 0) {
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
	mailbox_barrier();
}

void bcm2712_mailbox_send(mail_message_t* msg) {
	if (mailbox_wait_nonfull(MAILBOX_TIMEOUT_LOOPS) != 0) {
		return;
	}
	mailbox_barrier();
	mailbox_write_data_raw(mailbox_pack_message(msg));
}

void bcm2712_mailbox_call(mail_message_t* msg) {
	bcm2712_mailbox_send(msg);
	bcm2712_mailbox_read(msg);
}

int bcm2712_mailbox_call_timeout(mail_message_t* msg, uint32_t timeout_loops) {
	uint32_t attempts = timeout_loops == 0 ? MAILBOX_TIMEOUT_LOOPS : timeout_loops;
	uint32_t raw_req = mailbox_pack_message(msg);
	uint32_t raw_msg;

	if (mailbox_wait_nonfull(attempts) != 0) {
		return -1;
	}
	mailbox_barrier();
	mailbox_write_data_raw(raw_req);

	/*
	 * "The callee is not allowed to return a different buffer address, this
	 * allows the caller to make independent asynchronous requests." So the
	 * reply belonging to this request is the one echoing our own address;
	 * matching on the channel alone accepts a stale reply from an earlier
	 * request that timed out.
	 */
	for (uint32_t loops = attempts; loops > 0; --loops) {
		if (mailbox_read_status_raw() & MAILBOX_STATUS_EMPTY) {
			continue;
		}

		raw_msg = mailbox_read_data_raw();
		if (raw_msg == raw_req) {
			mailbox_barrier();
			mailbox_unpack_message(msg, raw_msg);
			return 0;
		}
	}
	return -1;
}
