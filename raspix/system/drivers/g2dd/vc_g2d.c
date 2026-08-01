#include "vc_g2d.h"

#include <string.h>
#include <arch/bcm283x/mailbox.h>
#include <ewoksys/dma.h>

/*
 * Hardware blt/scale through the VideoCore mailbox property interface.
 *
 * Works on both Raspberry Pi 3 (BCM2837) and 4 (BCM2711):
 *  - the mailbox register base is derived from _mmio_base, which comes from
 *    the kernel sysinfo (0x3F000000 on Pi3, 0xFE000000 on Pi4);
 *  - ARM<->VC address translation uses the legacy non-cached alias
 *    0x40000000, valid for the low 1GB on both SoCs (with 0xC0000000 as
 *    fallback alias, same policy as the framebuffer driver).
 */

#define VC_ALIAS_NONCACHED 0x40000000u
#define VC_ALIAS_COHERENT  0xC0000000u
#define VC_RESP_SUCCESS    0x80000000u

#define TAG_BLIT_IMAGE     0x0004000Au

/*
 * Blit message layout follows the framebuffer helper already present in
 * `arch_bcm283x/framebuffer.c`:
 *   [5..8]   src rect: x0, y0, x1, y1
 *   [9..10]  dst pos: x, y
 *   [11]     src colorspace (0 = RGB)
 *   [12]     dst colorspace (0 = RGB)
 *   [13]     flags (VC_G2D_BLIT_*)
 *   [14..17] src buffer: bus addr, width, height, pitch
 *   [18..21] dst buffer: bus addr, width, height, pitch
 *   [22]     end tag
 */
#define VC_BLIT_WORDS      24
#define VC_BLIT_VALUE_LEN  64u

static int vc_mbox_call_alias(uint32_t* buffer, uint32_t alias) {
	mail_message_t msg;
	memset(&msg, 0, sizeof(msg));
	msg.data = (dma_phy_addr(0, (ewokos_addr_t)buffer) + alias) >> 4;
	msg.channel = PROPERTY_CHANNEL;
	if (bcm283x_mailbox_call_timeout(&msg, 0) != 0)
		return -1;
	return (buffer[1] & VC_RESP_SUCCESS) != 0 ? 0 : -1;
}

static int vc_mbox_call(uint32_t* buffer, uint32_t words) {
	uint32_t size = words * sizeof(uint32_t);
	uint32_t* shadow = (uint32_t*)dma_alloc(0, size);

	if (shadow != NULL)
		memcpy(shadow, buffer, size);

	if (vc_mbox_call_alias(buffer, VC_ALIAS_NONCACHED) == 0) {
		if (shadow != NULL)
			dma_free(0, (ewokos_addr_t)shadow);
		return 0;
	}

	/* restore the request before retrying with the other alias */
	if (shadow != NULL) {
		memcpy(buffer, shadow, size);
		dma_free(0, (ewokos_addr_t)shadow);
	}

	if (vc_mbox_call_alias(buffer, VC_ALIAS_COHERENT) == 0)
		return 0;
	return -1;
}

int vc_g2d_init(void) {
	if (bcm283x_mailbox_init() == 0)
		return -1;
	return 0;
}

int vc_g2d_buf_alloc(vc_g2d_buf_t* buf, uint32_t size) {
	if (buf == NULL || size == 0)
		return -1;

	memset(buf, 0, sizeof(*buf));
	buf->vaddr = (uint32_t)dma_alloc(0, size);
	if (buf->vaddr == 0)
		return -1;
	buf->bus = dma_phy_addr(0, (ewokos_addr_t)buf->vaddr) + VC_ALIAS_NONCACHED;
	if (buf->bus == VC_ALIAS_NONCACHED) {
		dma_free(0, (ewokos_addr_t)buf->vaddr);
		memset(buf, 0, sizeof(*buf));
		return -1;
	}
	buf->size = size;
	return 0;
}

void vc_g2d_buf_free(vc_g2d_buf_t* buf) {
	if (buf == NULL)
		return;
	if (buf->vaddr != 0 && buf->size != 0)
		dma_free(0, (ewokos_addr_t)buf->vaddr);
	memset(buf, 0, sizeof(*buf));
}

void vc_g2d_buf_wrap(vc_g2d_buf_t* buf, uint32_t vaddr, uint32_t phy_base, uint32_t bus_base) {
	if (buf == NULL)
		return;
	memset(buf, 0, sizeof(*buf));
	buf->vaddr = vaddr;
	buf->bus = bus_base != 0 ? bus_base : (phy_base + VC_ALIAS_NONCACHED);
}

int vc_g2d_blit(const vc_g2d_buf_t* src,
		uint32_t src_w, uint32_t src_h, uint32_t src_pitch,
		int32_t sx, int32_t sy, int32_t sw, int32_t sh,
		const vc_g2d_buf_t* dst,
		uint32_t dst_w, uint32_t dst_h, uint32_t dst_pitch,
		int32_t dx, int32_t dy, int32_t dw, int32_t dh,
		uint32_t flags) {
	uint32_t* m;
	int32_t ret;

	if (src == NULL || dst == NULL || dst->bus == 0)
		return -1;
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return -1;
	if (sw != dw || sh != dh)
		return -1;

	m = (uint32_t*)dma_alloc(0, VC_BLIT_WORDS * sizeof(uint32_t));
	if (m == NULL)
		return -1;
	memset(m, 0, VC_BLIT_WORDS * sizeof(uint32_t));

	m[0] = VC_BLIT_WORDS * sizeof(uint32_t);
	m[1] = 0;
	m[2] = TAG_BLIT_IMAGE;
	m[3] = VC_BLIT_VALUE_LEN;
	m[4] = VC_BLIT_VALUE_LEN;

	m[5] = (uint32_t)sx;
	m[6] = (uint32_t)sy;
	m[7] = (uint32_t)(sx + sw);
	m[8] = (uint32_t)(sy + sh);

	m[9] = (uint32_t)dx;
	m[10] = (uint32_t)dy;
	m[11] = 0; /* src colorspace: RGB */
	m[12] = 0; /* dst colorspace: RGB */
	m[13] = flags;

	m[14] = src->bus;
	m[15] = src_w;
	m[16] = src_h;
	m[17] = src_pitch;

	m[18] = dst->bus;
	m[19] = dst_w;
	m[20] = dst_h;
	m[21] = dst_pitch;

	m[22] = 0; /* end tag */

	ret = vc_mbox_call(m, VC_BLIT_WORDS);
	dma_free(0, (ewokos_addr_t)m);
	return ret;
}

int vc_g2d_fill(const vc_g2d_buf_t* dst,
		uint32_t dst_w, uint32_t dst_h, uint32_t dst_pitch,
		int32_t x, int32_t y, int32_t w, int32_t h,
		uint32_t color) {
	vc_g2d_buf_t src;

	if (dst == NULL)
		return -1;

	memset(&src, 0, sizeof(src));
	src.bus = color; /* fill color travels in the src address field */
	return vc_g2d_blit(&src, 0, 0, 0,
			x, y, w, h,
			dst, dst_w, dst_h, dst_pitch,
			x, y, w, h,
			VC_G2D_BLIT_FILL);
}
