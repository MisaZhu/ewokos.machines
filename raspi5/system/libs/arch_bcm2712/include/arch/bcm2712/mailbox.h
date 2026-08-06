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

/*
 * VC bus alias OR'd into mailbox buffer addresses.
 *
 * bcm2712.dtsi declares a single RAM alias for the soc bus the VPU lives on:
 *   dma-ranges = <0xc0000000 0x00 0x00000000 0x40000000>,
 *                <0x7c000000 0x10 0x7c000000 0x04000000>;
 * so ARM physical 0..1GB is reachable at bus 0xC0000000, and buffers handed
 * to the firmware must sit inside that window.
 */
#define MAILBOX_VC_ALIAS_RAM  0xC0000000u
#define MAILBOX_VC_RAM_WINDOW 0x40000000u

/*
 * BCM283x used 0x40000000 for the L2-coherent alias, and the kernel side of
 * this port still talks to the mailbox through it. Firmware builds differ in
 * how strictly they decode the alias bits, so it stays available as a probe
 * fallback for the first property call.
 */
#define MAILBOX_VC_ALIAS_LEGACY 0x40000000u

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
/* returns 0 only when the firmware answered with the very buffer address that
 * was submitted, as required by the property interface contract */
int      bcm2712_mailbox_call_timeout(mail_message_t* msg, uint32_t timeout_loops);

#endif
