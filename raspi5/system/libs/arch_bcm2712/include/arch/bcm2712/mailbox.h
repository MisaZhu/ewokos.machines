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
#define MAIL0_READ         (((volatile mail_message_t *)(0x00 + MAILBOX_BASE)))
#define MAIL0_STATUS       (((volatile mail_status_t *)(0x18 + MAILBOX_BASE)))
#define MAIL0_WRITE        (((volatile mail_message_t *)(0x20 + MAILBOX_BASE)))
#define PROPERTY_CHANNEL   8
#define FRAMEBUFFER_CHANNEL 1

typedef struct {
	uint8_t channel: 4;
	uint32_t data: 28;
} mail_message_t;

typedef struct {
	uint32_t reserved: 30;
	uint8_t empty: 1;
	uint8_t full:1;
} mail_status_t;

void     bcm2712_mailbox_read(mail_message_t* msg);
void     bcm2712_mailbox_send(mail_message_t* msg);
void     bcm2712_mailbox_call(mail_message_t* msg);
int      bcm2712_mailbox_call_timeout(mail_message_t* msg, uint32_t timeout_loops);

#endif
