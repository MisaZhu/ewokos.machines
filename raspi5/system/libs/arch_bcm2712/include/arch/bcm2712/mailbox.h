#ifndef BCM2712_MAILBOX_H
#define BCM2712_MAILBOX_H

#include <stdint.h>
#include <ewoksys/mmio.h>

/*
 * VPU property mailbox interface (legacy, still serviced by firmware on BCM2712).
 *
 * Mailbox registers are inside the main peripheral window at offset 0x00013880,
 * accessed relative to _mmio_base (set by mmio_map()).
 */

#define PI5_MAILBOX_OFF    0x00013880

#define MAILBOX_BASE       (_mmio_base + PI5_MAILBOX_OFF)
#define PROPERTY_CHANNEL   8
#define FRAMEBUFFER_CHANNEL 1

/* VC bus address aliases OR'd into mailbox buffer addresses. */
#define MAILBOX_VC_ALIAS_NONCACHED 0x40000000u
#define MAILBOX_VC_ALIAS_COHERENT  0xC0000000u

/* Set in the property buffer response word on success. */
#define MAILBOX_RESPONSE_SUCCESS 0x80000000u

typedef struct {
	uint8_t channel: 4;
	uint32_t data: 28;
} mail_message_t;

ewokos_addr_t bcm2712_mailbox_init(void);
void     bcm2712_mailbox_read(mail_message_t* msg);
void     bcm2712_mailbox_send(mail_message_t* msg);
void     bcm2712_mailbox_call(mail_message_t* msg);
int      bcm2712_mailbox_call_timeout(mail_message_t* msg, uint32_t timeout_loops);

#endif
