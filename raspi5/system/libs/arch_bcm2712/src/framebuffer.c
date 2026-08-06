#include <stdio.h>
#include <string.h>
#include <arch/bcm2712/mailbox.h>
#include <arch/bcm2712/framebuffer.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <sysinfo.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>

static fbinfo_t _fb_info;
static uint32_t _fb_pixel_order = PIXEL_ORDER_BGR;
static uint32_t _fb_alpha_mode = ALPHA_MODE_IGNORED;
static uint32_t _vc_alias = MAILBOX_VC_ALIAS_RAM;
/* scan-out was allocated by this driver, not by the VPU */
static int _fb_driver_owned = 0;

#define TAG_SET_PHYS_SIZE      0x00048003
#define TAG_SET_VIRT_SIZE      0x00048004
#define TAG_SET_DEPTH          0x00048005
#define TAG_SET_VIRTUAL_OFFSET 0x00048009
#define TAG_SET_PITCH          0x00048008
#define TAG_SET_DISPLAY_NUM    0x00048013
#define TAG_ALLOCATE_FB        0x00040001
#define TAG_GET_PITCH          0x00040008
#define TAG_GET_NUM_DISPLAYS   0x00040013

/* GET variants: request_size=0, firmware returns the current value. */
#define TAG_GET_PHYS_SIZE   0x00040003
#define TAG_GET_VIRT_SIZE   0x00040004
#define TAG_GET_DEPTH       0x00040005
#define TAG_GET_PIXEL_ORDER 0x00040006
#define TAG_GET_ALPHA_MODE  0x00040007

#define TAG_RESPONSE_BIT    0x80000000u

#define QUERY_REQ_WORDS  25
#define ALLOC_REQ_WORDS  8
#define MODE_REQ_WORDS   31
#define SIMPLE_REQ_WORDS 7
#define DIM_REQ_WORDS    8

static uint32_t align_up(uint32_t value, uint32_t align) {
	return (value + align - 1) & (~(align - 1));
}

/*
 * Property mailbox call.
 *
 * bcm2712.dtsi declares exactly one RAM alias for the VPU:
 *   soc { dma-ranges = <0xc0000000 0x00 0x00000000 0x40000000>, ... };
 * i.e. the firmware sees ARM physical 0..1GB through bus 0xC0000000, and
 * nothing else. So the request buffer must live in the low 1GB.
 */
static int mailbox_property_send(uint32_t* buffer, uint32_t alias) {
	ewokos_addr_t phy = dma_phy_addr(0, (ewokos_addr_t)buffer);
	mail_message_t msg;

	if (phy == 0 || (phy + buffer[0]) > MAILBOX_VC_RAM_WINDOW) {
		klog("fb: mailbox buffer phy=%x outside the VPU 1GB window\n", (uint32_t)phy);
		return -1;
	}

	memset(&msg, 0, sizeof(mail_message_t));
	msg.data = ((uint32_t)phy | alias) >> 4;
	msg.channel = PROPERTY_CHANNEL;
	if (bcm2712_mailbox_call_timeout(&msg, 0) != 0) {
		return -1;
	}
	return (buffer[1] & MAILBOX_RESPONSE_SUCCESS) != 0 ? 0 : -1;
}

static int mailbox_property_call(uint32_t* buffer) {
	uint32_t request_size = buffer[0];

	if (mailbox_property_send(buffer, _vc_alias) == 0) {
		return 0;
	}

	/* first call only: the alias the firmware decodes is not known yet */
	if (_vc_alias == MAILBOX_VC_ALIAS_RAM) {
		buffer[0] = request_size;
		buffer[1] = 0;
		if (mailbox_property_send(buffer, MAILBOX_VC_ALIAS_LEGACY) == 0) {
			_vc_alias = MAILBOX_VC_ALIAS_LEGACY;
			klog("fb: firmware answers on the legacy 0x40000000 vc alias\n");
			return 0;
		}
	}
	return -1;
}

/* a tag the firmware actually handled has bit31 set in its code word */
static int tag_answered(const uint32_t* req, uint32_t code_idx) {
	return (req[code_idx] & TAG_RESPONSE_BIT) != 0;
}

/* ─── framebuffer validation & adoption ─── */

/*
 * Validate a firmware-reported mode and publish it.
 *
 * own_ptr != 0 means the driver allocated the scan-out itself, so it is
 * already mapped Normal-NonCacheable by SYS_DMA_ALLOC and must not be mapped
 * again. own_ptr == 0 is the VPU-allocated case: the buffer lives in the
 * firmware reserve and has to be brought in with SYS_MEM_MAP.
 */
static int fb_adopt(const sys_info_t* sysinfo, uint32_t w, uint32_t h,
		uint32_t vw, uint32_t vh, uint32_t dep, uint32_t order, uint32_t alpha,
		uint32_t bus, uint32_t size, uint32_t pitch, ewokos_addr_t own_ptr,
		fbinfo_t* info) {
	/* strip the VC alias bits to get the ARM physical address */
	ewokos_addr_t phy = bus & (MAILBOX_VC_RAM_WINDOW - 1);

	if ((w == 0) || (h == 0) || (bus == 0) || (size == 0)) {
		return -1;
	}
	if (dep != 16 && dep != 32) {
		klog("fb: adopt bad depth %u\n", dep);
		return -1;
	}

	memset(info, 0, sizeof(fbinfo_t));
	info->width = w;
	info->height = h;
	info->vwidth = vw != 0 ? vw : w;
	info->vheight = vh != 0 ? vh : h;
	info->depth = dep;
	info->pitch = pitch != 0 ? pitch : (info->vwidth * (info->depth / 8));
	info->phy_base = phy;
	info->bus_base = bus;
	info->size = size;
	info->xoffset = 0;
	info->yoffset = 0;
	info->size_max = align_up(size, 4096);
	info->dma_id = -1;

	if (own_ptr != 0) {
		info->pointer = own_ptr;
	}
	else {
		info->pointer = sysinfo->sys_dma.v_base + sysinfo->sys_dma.size;
		if (syscall3(SYS_MEM_MAP,
				(ewokos_addr_t)info->pointer,
				(ewokos_addr_t)info->phy_base,
				(ewokos_addr_t)info->size_max) == 0) {
			klog("fb: mem_map fail v=%x phy=%x size=%u\n",
					info->pointer, info->phy_base, info->size_max);
			memset(info, 0, sizeof(fbinfo_t));
			return -1;
		}
	}

	_fb_pixel_order = (order == PIXEL_ORDER_RGB) ? PIXEL_ORDER_RGB : PIXEL_ORDER_BGR;
	_fb_alpha_mode = alpha;
	return 0;
}

/*
 * Number of displays the firmware drives, 1 on failure.
 *
 * Pi 4 and Pi 5 have two HDMI ports, so every framebuffer tag applies to
 * whichever display is currently selected. Circle (rsta2/circle,
 * lib/bcmframebuffer.cpp) queries this and then selects one before touching
 * anything else; without that the tags below silently act on the wrong port.
 */
static uint32_t fb_num_displays(void) {
	uint32_t* req = (uint32_t*)dma_alloc(0, SIMPLE_REQ_WORDS * sizeof(uint32_t));
	uint32_t n = 1;

	if (req == NULL) {
		return n;
	}

	memset(req, 0, SIMPLE_REQ_WORDS * sizeof(uint32_t));
	req[0] = SIMPLE_REQ_WORDS * sizeof(uint32_t);
	req[1] = 0;
	req[2] = TAG_GET_NUM_DISPLAYS;
	req[3] = 4;
	req[4] = 0;
	req[5] = 0;
	req[6] = 0; /* end tag */

	if (mailbox_property_call(req) == 0 && tag_answered(req, 4) && req[5] != 0) {
		n = req[5];
	}

	dma_free(0, (ewokos_addr_t)req);
	return n;
}

/* Select the display all following framebuffer tags apply to. */
static int fb_set_display(uint32_t num) {
	uint32_t* req = (uint32_t*)dma_alloc(0, SIMPLE_REQ_WORDS * sizeof(uint32_t));
	int ret;

        if (req == NULL) {
		return -1;
	}

	memset(req, 0, SIMPLE_REQ_WORDS * sizeof(uint32_t));
	req[0] = SIMPLE_REQ_WORDS * sizeof(uint32_t);
	req[1] = 0;
	req[2] = TAG_SET_DISPLAY_NUM;
	req[3] = 4;
	req[4] = 4;
	req[5] = num;
	req[6] = 0; /* end tag */

	ret = (mailbox_property_call(req) == 0 && tag_answered(req, 4)) ? 0 : -1;
	dma_free(0, (ewokos_addr_t)req);
	return ret;
}

/*
 * Mode the firmware set up from config.txt, so the driver can keep it instead
 * of forcing an unrelated resolution.
 *
 * Circle sanity checks this against 640..4096 x 480..2160, but that rejects
 * portrait panels: an hdmi0_cvt of 480x800 is only 480 wide. Only obviously
 * bogus values are refused here.
 */
static int fb_display_dimensions(uint32_t* w, uint32_t* h) {
	uint32_t* req = (uint32_t*)dma_alloc(0, DIM_REQ_WORDS * sizeof(uint32_t));
	int ret = -1;

	if (req == NULL) {
		return -1;
	}

	memset(req, 0, DIM_REQ_WORDS * sizeof(uint32_t));
	req[0] = DIM_REQ_WORDS * sizeof(uint32_t);
	req[1] = 0;
	req[2] = TAG_GET_PHYS_SIZE; /* GET_DISPLAY_DIMENSIONS */
	req[3] = 8;
	req[4] = 0;
	req[5] = 0;
	req[6] = 0;
	req[7] = 0; /* end tag */

	if (mailbox_property_call(req) == 0 && tag_answered(req, 4) &&
			req[5] >= 64 && req[5] <= 4096 &&
			req[6] >= 64 && req[6] <= 4096) {
		*w = req[5];
		*h = req[6];
		ret = 0;
	}

	dma_free(0, (ewokos_addr_t)req);
	return ret;
}

/*
 * Last resort: adopt the scan-out buffer the firmware programmed at boot,
 * without touching the mode at all.
 *
 * ALLOCATE_BUFFER is sent on its own in the documented 4 byte form, where the
 * request value is a single u32 alignment and the response is base+size. With
 * no preceding SET_* tags the firmware hands back the buffer it is already
 * scanning out instead of allocating a new one.
 */
static int fb_query_buffer(uint32_t* bus, uint32_t* size) {
	uint32_t* req = (uint32_t*)dma_alloc(0, ALLOC_REQ_WORDS * sizeof(uint32_t));

        if (req == NULL) {
		return -1;
	}

	memset(req, 0, ALLOC_REQ_WORDS * sizeof(uint32_t));
	req[0] = ALLOC_REQ_WORDS * sizeof(uint32_t);
	req[1] = 0;
	req[2] = TAG_ALLOCATE_FB;
	req[3] = 8; /* value buffer: u32 base + u32 size */
	req[4] = 4; /* request: u32 alignment */
	req[5] = 16; /* alignment -> response: base */
	req[6] = 0; /* response: size */
	req[7] = 0; /* end tag */

	if (mailbox_property_call(req) != 0 || !tag_answered(req, 4)) {
		dma_free(0, (ewokos_addr_t)req);
		return -1;
	}

	*bus = req[5];
	*size = req[6];
	dma_free(0, (ewokos_addr_t)req);
	return (*bus != 0 && *size != 0) ? 0 : -1;
}

static int fb_query_mode(uint32_t* w, uint32_t* h, uint32_t* vw, uint32_t* vh,
		uint32_t* dep, uint32_t* order, uint32_t* alpha, uint32_t* pitch) {
	uint32_t* req = (uint32_t*)dma_alloc(0, QUERY_REQ_WORDS * sizeof(uint32_t));

	if (req == NULL) {
		return -1;
	}

	memset(req, 0, QUERY_REQ_WORDS * sizeof(uint32_t));
	req[0] = QUERY_REQ_WORDS * sizeof(uint32_t);
	req[1] = 0;
	req[2] = TAG_GET_PHYS_SIZE;
	req[3] = 8;
	req[4] = 0;
	req[7] = TAG_GET_VIRT_SIZE;
	req[8] = 8;
	req[9] = 0;
	req[12] = TAG_GET_DEPTH;
	req[13] = 4;
	req[14] = 0;
	req[16] = TAG_GET_PIXEL_ORDER;
	req[17] = 4;
	req[18] = 0;
	req[20] = TAG_GET_ALPHA_MODE;
	req[21] = 4;
	req[22] = 0;
	/* req[15] = depth, req[19] = order, req[23] = alpha, req[24] = end tag */

	if (mailbox_property_call(req) != 0) {
		dma_free(0, (ewokos_addr_t)req);
		return -1;
	}

	*w = req[5];
	*h = req[6];
	*vw = req[10];
	*vh = req[11];
	*dep = req[15];
	*order = req[19];
	/* an unanswered alpha tag must not be read as ALPHA_MODE_ENABLED (0) */
	*alpha = tag_answered(req, 22) ? req[23] : ALPHA_MODE_IGNORED;
	dma_free(0, (ewokos_addr_t)req);

	/* pitch is asked for separately: it is only meaningful once the mode
	 * above is known good, and mixing it in would grow the buffer again */
	*pitch = 0;
	req = (uint32_t*)dma_alloc(0, ALLOC_REQ_WORDS * sizeof(uint32_t));
	if (req != NULL) {
		memset(req, 0, ALLOC_REQ_WORDS * sizeof(uint32_t));
		req[0] = 7 * sizeof(uint32_t);
		req[1] = 0;
		req[2] = TAG_GET_PITCH;
		req[3] = 4;
		req[4] = 0;
		req[5] = 0;
		req[6] = 0; /* end tag */
		if (mailbox_property_call(req) == 0 && tag_answered(req, 4)) {
			*pitch = req[5];
		}
		dma_free(0, (ewokos_addr_t)req);
	}

	return (*w != 0 && *h != 0) ? 0 : -1;
}

static int fb_adopt_firmware(const sys_info_t* sysinfo, fbinfo_t* info) {
	uint32_t bus = 0, size = 0, pitch = 0;
	uint32_t w = 0, h = 0, vw = 0, vh = 0, dep = 0;
	uint32_t order = PIXEL_ORDER_BGR, alpha = ALPHA_MODE_IGNORED;

	if (fb_query_buffer(&bus, &size) != 0) {
		klog("fb: firmware has no scanout buffer\n");
		return -1;
	}
	if (fb_query_mode(&w, &h, &vw, &vh, &dep, &order, &alpha, &pitch) != 0) {
		klog("fb: firmware mode query failed (bus=%x size=%u)\n", bus, size);
		return -1;
	}

	if (fb_adopt(sysinfo, w, h, vw, vh, dep, order, alpha, bus, size, pitch, 0, info) != 0) {
		klog("fb: adopt rejected %ux%u@%u bus=%x size=%u pitch=%u order=%u\n",
				w, h, dep, bus, size, pitch, order);
		return -1;
	}
	return 0;
}

typedef struct {
	uint32_t w, h, vw, vh, dep, bus, size, pitch;
} fb_mode_reply_t;

typedef struct {
        uint32_t width;
        uint32_t height;
        uint32_t depth;
} fb_mode_t;

static int fb_mode_equal(const fb_mode_t* a, const fb_mode_t* b) {
        return a->width == b->width &&
                        a->height == b->height &&
                        a->depth == b->depth;
}

/*
 * One property message carrying the whole mode, laid out exactly like
 * struct fb_alloc_tags in linux drivers/video/fbdev/bcm2708_fb.c:
 *
 *   SET_PHYSICAL_WIDTH_HEIGHT, SET_VIRTUAL_WIDTH_HEIGHT, SET_DEPTH,
 *   SET_VIRTUAL_OFFSET, FRAMEBUFFER_ALLOCATE, SET_PITCH | GET_PITCH
 *
 * bus != 0 is the scheme linux prefers (bcm2708_fb_set_par): the driver owns
 * the scan-out memory and hands its bus address plus byte size to
 * FRAMEBUFFER_ALLOCATE, together with SET_PITCH. bus == 0 is what linux calls
 * the old scheme: base and screen_size are left at 0 so the VPU allocates,
 * and the pitch is read back with GET_PITCH.
 *
 * Every tag carries request size 0, again like linux: the firmware takes the
 * request length from the value buffer size. That matters for
 * FRAMEBUFFER_ALLOCATE, whose two forms are told apart by base being zero or
 * not - the 4 byte "alignment" form would make the first word an alignment
 * instead of the buffer we want scanned out.
 *
 * SET_VIRTUAL_OFFSET has to stay in the chain: the firmware keeps whatever
 * offset it was left with, and a stale non-zero one shifts the visible window
 * inside the virtual framebuffer. Depth 32 alone means ARGB8888, so neither
 * SET_PIXEL_ORDER nor SET_ALPHA_MODE is sent - linux does not send them
 * either, and both disturb a running scan-out.
 *
 * The caller must have selected the display first.
 */
static int fb_mode_call(fb_mode_reply_t* r, uint32_t w, uint32_t h, uint32_t dep,
		uint32_t bus, uint32_t size, uint32_t pitch) {
	uint32_t* req = (uint32_t*)dma_alloc(0, MODE_REQ_WORDS * sizeof(uint32_t));

	if (req == NULL) {
		return -1;
	}

	memset(req, 0, MODE_REQ_WORDS * sizeof(uint32_t));
	req[0] = MODE_REQ_WORDS * sizeof(uint32_t);
	req[1] = 0;
	req[2] = TAG_SET_PHYS_SIZE;
	req[3] = 8;
	req[4] = 0;
	req[5] = w;
	req[6] = h;
	req[7] = TAG_SET_VIRT_SIZE;
	req[8] = 8;
	req[9] = 0;
	req[10] = w;
	req[11] = h;
	req[12] = TAG_SET_DEPTH;
	req[13] = 4;
	req[14] = 0;
	req[15] = dep;
	req[16] = TAG_SET_VIRTUAL_OFFSET;
	req[17] = 8;
	req[18] = 0;
	req[19] = 0;
	req[20] = 0;
	req[21] = TAG_ALLOCATE_FB;
	req[22] = 8;
	req[23] = 0;
	req[24] = bus;  /* -> response: base */
	req[25] = size; /* -> response: size */
	req[26] = (bus != 0) ? TAG_SET_PITCH : TAG_GET_PITCH;
	req[27] = 4;
	req[28] = 0;
	req[29] = pitch;
	req[30] = 0; /* end tag */

	if (mailbox_property_call(req) != 0) {
		dma_free(0, (ewokos_addr_t)req);
		return -1;
	}

	r->w = req[5];
	r->h = req[6];
	r->vw = req[10];
	r->vh = req[11];
	r->dep = req[15];
	r->bus = req[24];
	r->size = req[25];
	r->pitch = req[29];
	dma_free(0, (ewokos_addr_t)req);
	return 0;
}

/*
 * Set the mode and get a scan-out buffer, preferring the buffer this driver
 * owns.
 *
 * Handing the firmware a buffer we allocated is what linux does first, and it
 * is the only way to be sure the scan-out is memory nobody else can hand out:
 * the sys_dma pool is carved off before the kernel heap is built
 * (kernel/kernel/src/hw_info.c advances allocable_phy_mem_base past it), it is
 * inside the low 1GB the VPU can reach, and SYS_DMA_ALLOC already maps it
 * Normal-NonCacheable, which is exactly what a scan-out wants.
 *
 * Guessing the firmware's own buffer address instead means mapping memory the
 * kernel may also have put in its heap, and a scan-out full of somebody
 * else's freelist is indistinguishable from a broken display.
 *
 * If the firmware answers with a different base it does not support being
 * given an allocation (linux sets disable_arm_alloc at that point), so the
 * buffer is released and the VPU-allocated form is used instead.
 */
static int fb_alloc_mode(const sys_info_t* sysinfo, uint32_t w, uint32_t h,
		uint32_t dep, fbinfo_t* info) {
	fb_mode_reply_t r;
	uint32_t pitch = w * (dep / 8);
	uint32_t image_size = pitch * h;
	ewokos_addr_t own_v = dma_alloc(0, image_size);
	ewokos_addr_t own_phy = own_v != 0 ? dma_phy_addr(0, own_v) : 0;

	if (own_v != 0 &&
			(own_phy == 0 || (own_phy + image_size) > MAILBOX_VC_RAM_WINDOW)) {
		klog("fb: dma phy=%x size=%u outside the VPU 1GB window\n",
				(uint32_t)own_phy, image_size);
		dma_free(0, own_v);
		own_v = 0;
	}

	if (own_v != 0) {
		uint32_t bus = (uint32_t)own_phy | _vc_alias;

		memset(&r, 0, sizeof(fb_mode_reply_t));
		if (fb_mode_call(&r, w, h, dep, bus, image_size, pitch) == 0 &&
				(r.bus & (MAILBOX_VC_RAM_WINDOW - 1)) == (uint32_t)own_phy &&
				fb_adopt(sysinfo, r.w, r.h, r.vw, r.vh, r.dep,
					PIXEL_ORDER_BGR, ALPHA_MODE_IGNORED, bus, image_size,
					r.pitch != 0 ? r.pitch : pitch, own_v, info) == 0) {
			_fb_driver_owned = 1;
			return 0;
		}
		klog("fb: firmware kept its own buffer (asked %x, got %x)\n",
				bus, r.bus);
		dma_free(0, own_v);
	}

	memset(&r, 0, sizeof(fb_mode_reply_t));
	if (fb_mode_call(&r, w, h, dep, 0, 0, 0) != 0) {
		return -1;
	}
	if (r.w == 0 || r.h == 0 || r.vw == 0 || r.vh == 0 || r.dep == 0 ||
			r.bus == 0 || r.size == 0) {
		klog("fb: alloc %ux%u@%u refused (w=%u h=%u vw=%u vh=%u dep=%u base=%x size=%u)\n",
				w, h, dep, r.w, r.h, r.vw, r.vh, r.dep, r.bus, r.size);
		return -1;
	}

	if (fb_adopt(sysinfo, r.w, r.h, r.vw, r.vh, r.dep, PIXEL_ORDER_BGR,
			ALPHA_MODE_IGNORED, r.bus, r.size, r.pitch, 0, info) != 0) {
		klog("fb: alloc rejected %ux%u@%u base=%x size=%u\n",
				r.w, r.h, r.dep, r.bus, r.size);
		return -1;
	}
	return 0;
}

/*
 * fb_try_mode_list — try a requested mode followed by a fallback list
 * using a given strategy function. Returns 0 on first success.
 */
typedef int (*fb_init_fn_t)(const sys_info_t *, const fb_mode_t *, fbinfo_t *);

static int fb_try_mode_list(const sys_info_t *sysinfo,
		const fb_mode_t *requested,
		const fb_mode_t *fallbacks, uint32_t n_fallbacks,
		fb_init_fn_t init_fn, const char *strat_name,
		fbinfo_t *info) {
	if (init_fn(sysinfo, requested, info) == 0) {
		return 0;
	}

	for (uint32_t i = 0; i < n_fallbacks; ++i) {
		if (fb_mode_equal(requested, &fallbacks[i])) {
			continue;
		}
		if (init_fn(sysinfo, &fallbacks[i], info) == 0) {
			return 0;
		}
	}

	return -1;
}

/* ─── public API ─── */

int32_t bcm2712_fb_init(uint32_t w, uint32_t h, uint32_t dep) {
	sys_info_t sysinfo;
	uint32_t displays;

	memset(&_fb_info, 0, sizeof(fbinfo_t));
	_fb_pixel_order = PIXEL_ORDER_BGR;
	_fb_alpha_mode = ALPHA_MODE_IGNORED;
	_fb_driver_owned = 0;
	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);

	if (bcm2712_mailbox_init() == 0) {
		klog("fb_init: mmio map failed\n");
		return -1;
	}

	/*
	 * Pi 5 drives two HDMI ports, so a display has to be selected before any
	 * framebuffer tag is sent, otherwise the firmware applies them to whatever
	 * was current. Display 0 is hdmi0, which is what config.txt's hdmi0_*
	 * settings configure.
	 */
	displays = fb_num_displays();
	if (fb_set_display(0) != 0) {
		klog("fb_init: cannot select display 0 of %u\n", displays);
	}

	if (dep != 16 && dep != 32) {
		dep = 32;
	}
	/*
	 * No resolution configured: keep whatever config.txt set up rather than
	 * forcing an unrelated mode.
	 */
	if (w == 0 || h == 0) {
		if (fb_display_dimensions(&w, &h) != 0) {
			w = 640;
			h = 480;
		}
	}

	if (fb_alloc_mode(&sysinfo, w, h, dep, &_fb_info) != 0 &&
			fb_adopt_firmware(&sysinfo, &_fb_info) != 0) {
		klog("fb_init: no framebuffer, check the hdmi0_* settings in config.txt\n");
		return -1;
	}

	klog("fb_init: %ux%u@%u pitch=%u phy=%x bus=%x size=%u order=%s alpha=%u displays=%u owner=%s\n",
			_fb_info.width, _fb_info.height, _fb_info.depth,
			_fb_info.pitch, _fb_info.phy_base, _fb_info.bus_base,
			_fb_info.size, _fb_pixel_order == PIXEL_ORDER_RGB ? "rgb" : "bgr",
			_fb_alpha_mode, displays, _fb_driver_owned ? "driver" : "vpu");
	if (_fb_info.depth == 32 && _fb_pixel_order != PIXEL_ORDER_BGR) {
		klog("fb_init: scanout is not ARGB8888, colours may be wrong\n");
	}
	return 0;
}

fbinfo_t *bcm2712_get_fbinfo(void) {
	return &_fb_info;
}

uint32_t bcm2712_fb_pixel_order(void) {
	return _fb_pixel_order;
}

uint32_t bcm2712_fb_alpha_mode(void) {
	return _fb_alpha_mode;
}
