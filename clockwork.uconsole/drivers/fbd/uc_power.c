#include "uc_power.h"

#include <string.h>

#include <arch/bcm283x/mailbox.h>
#include <ewoksys/dma.h>

#include "uc_log.h"
#define slog uc_log

/*
 * VC property mailbox tags (raspberrypi-firmware.h).
 * SET_DOMAIN_STATE is the "new" power interface that covers DSI1;
 * the old SET_POWER_STATE interface only knows USB/V3D-style device
 * ids and cannot power the DSI domains.
 */
#define TAG_GET_DOMAIN_STATE  0x00030030U
#define TAG_SET_DOMAIN_STATE  0x00038030U

#define MAILBOX_VC_ALIAS_NONCACHED  0x40000000U
#define MAILBOX_VC_ALIAS_COHERENT   0xC0000000U
#define MAILBOX_RESPONSE_SUCCESS    0x80000000U

static int _mbox_call(uint32_t* buf, uint32_t alias) {
	mail_message_t msg;
	memset(&msg, 0, sizeof(msg));
	msg.data = (dma_phy_addr(0, (ewokos_addr_t)(uintptr_t)buf) + alias) >> 4;
	msg.channel = PROPERTY_CHANNEL;
	if (bcm283x_mailbox_call_timeout(&msg, 0) != 0) {
		return -1;
	}
	return (buf[1] & MAILBOX_RESPONSE_SUCCESS) != 0 ? 0 : -1;
}

/*
 * One-tag property transaction with the same VC bus-alias fallback the
 * rest of raspix uses (framebuffer.c): try the non-cached alias first,
 * retry with the coherent alias if the firmware stays silent.
 */
static int _property_call(uint32_t tag, uint32_t* val0, uint32_t* val1) {
	uint32_t size = 8 * 4;
	uint32_t* buf = (uint32_t*)(uintptr_t)dma_alloc(0, size);
	int ret = -1;
	int attempt;

	if (buf == NULL) {
		return -1;
	}

	for (attempt = 0; attempt < 2 && ret != 0; attempt++) {
		uint32_t alias = (attempt == 0) ?
				MAILBOX_VC_ALIAS_NONCACHED : MAILBOX_VC_ALIAS_COHERENT;
		buf[0] = size;
		buf[1] = 0;              /* process request */
		buf[2] = tag;
		buf[3] = 8;              /* value buffer size */
		buf[4] = 8;              /* request: value length */
		buf[5] = *val0;
		buf[6] = *val1;
		buf[7] = 0;              /* end tag */
		ret = _mbox_call(buf, alias);
	}

	if (ret == 0) {
		*val0 = buf[5];
		*val1 = buf[6];
	}
	dma_free(0, (ewokos_addr_t)(uintptr_t)buf);
	return ret;
}

int uc_power_domain_set(uint32_t domain, int on) {
	uint32_t d;
	uint32_t state;

	/* Request the state change. */
	d = domain;
	state = on ? 1U : 0U;
	if (_property_call(TAG_SET_DOMAIN_STATE, &d, &state) != 0) {
		slog("[uc_power] SET_DOMAIN_STATE %u failed\n", domain);
		return -1;
	}

	/*
	 * Read the state back: old firmware silently skips unknown tags,
	 * so a successful mailbox roundtrip alone doesn't prove the
	 * domain moved.
	 */
	d = domain;
	state = 0xffffffffU;
	if (_property_call(TAG_GET_DOMAIN_STATE, &d, &state) != 0) {
		return -1;
	}
	if (on && (state & 1U) == 0U) {
		slog("[uc_power] domain %u still off after enable\n", domain);
		return -1;
	}
	slog("[uc_power] domain %u -> %s\n", domain, on ? "on" : "off");
	return 0;
}

int uc_power_domain_get(uint32_t domain) {
	uint32_t d = domain;
	uint32_t state = 0xffffffffU;

	if (_property_call(TAG_GET_DOMAIN_STATE, &d, &state) != 0) {
		return -1;
	}
	return (int)(state & 1U);
}
