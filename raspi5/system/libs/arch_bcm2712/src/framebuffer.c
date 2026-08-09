#include <stdio.h>
#include <string.h>
#include <arch/bcm2712/mailbox.h>
#include <arch/bcm2712/framebuffer.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <sysinfo.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <arch/bcm2712/native_hdmi.h>

static fbinfo_t _fb_info;

/* ─── structured property tag types (mirrors Circle's bcmpropertytags.h) ─── */

#define PROPTAG_END                    0x00000000u

/* GET tags (request_size=0, firmware returns current value) */
#define PROPTAG_GET_NUM_DISPLAYS       0x00040013u
#define PROPTAG_GET_PHYS_WIDTH_HEIGHT  0x00040003u
#define PROPTAG_GET_VIRT_WIDTH_HEIGHT  0x00040004u
#define PROPTAG_GET_DEPTH              0x00040005u
#define PROPTAG_GET_PIXEL_ORDER        0x00040006u
#define PROPTAG_GET_PITCH              0x00040008u
#define PROPTAG_GET_DISPLAY_DIMENSIONS 0x00040003u  /* same ID as PHYS, different semantic */

/* SET tags */
#define PROPTAG_SET_DISPLAY_NUM        0x00048013u
#define PROPTAG_SET_PHYS_WIDTH_HEIGHT  0x00048003u
#define PROPTAG_SET_VIRT_WIDTH_HEIGHT  0x00048004u
#define PROPTAG_SET_DEPTH              0x00048005u
#define PROPTAG_SET_PIXEL_ORDER        0x00048006u
#define PROPTAG_SET_VIRTUAL_OFFSET     0x00048009u
#define PROPTAG_ALLOCATE_BUFFER        0x00040001u

#define PIXEL_ORDER_BGR                0u

#define VALUE_LENGTH_RESPONSE          (1u << 31)

/* Circle-aligned: generic tag header */
typedef struct {
	uint32_t tag_id;
	uint32_t value_buf_size;  /* bytes, must be multiple of 4 */
	uint32_t value_length;    /* bytes; bit 31 set = response */
} __attribute__((packed)) prop_tag_t;

/* Tag carrying a single u32 value (depth, pitch, pixel order, etc.) */
typedef struct {
	prop_tag_t tag;
	uint32_t   value;
} __attribute__((packed)) prop_tag_simple_t;

/* Tag carrying width + height (phys / virt / display dimensions) */
typedef struct {
	prop_tag_t tag;
	uint32_t   width;
	uint32_t   height;
} __attribute__((packed)) prop_tag_dims_t;

/* ALLOCATE_BUFFER tag */
typedef struct {
	prop_tag_t tag;
	uint32_t   alignment_or_base; /* input: alignment, output: bus address */
	uint32_t   size;              /* input: 0=query, output: allocated size */
} __attribute__((packed)) prop_tag_alloc_t;

/* ─── tag block structs for init / query (one-shot GetTags call) ─── */

/*
 * fb_init_tags_t — allocate a new framebuffer (SET tags + ALLOCATE).
 * Mirrors Circle's TBcmFrameBufferInitTags.
 */
typedef struct {
	prop_tag_simple_t set_display_num;
	prop_tag_dims_t   set_phys;
	prop_tag_dims_t   set_virt;
	prop_tag_simple_t set_depth;
	prop_tag_simple_t set_pixel_order;
	prop_tag_dims_t   set_offset;
	prop_tag_alloc_t  allocate;
	prop_tag_simple_t get_pitch;
} __attribute__((packed)) fb_init_tags_t;

/*
 * fb_query_tags_t — adopt the firmware's existing framebuffer
 * (GET tags + ALLOCATE with 0 request bytes).
 */
typedef struct {
	prop_tag_simple_t set_display_num;
	prop_tag_dims_t   get_phys;
	prop_tag_dims_t   get_virt;
	prop_tag_simple_t get_depth;
	prop_tag_simple_t get_pixel_order;
	prop_tag_alloc_t  allocate;
	prop_tag_simple_t get_pitch;
} __attribute__((packed)) fb_query_tags_t;

/* Simple tags block for single-tag queries like GET_DISPLAY_DIMENSIONS */
typedef struct {
	prop_tag_simple_t get_num_displays;
} __attribute__((packed)) fb_num_displays_tags_t;

typedef struct {
	prop_tag_simple_t set_display_num;
	prop_tag_dims_t   dims;
} __attribute__((packed)) fb_dims_tags_t;

/* ─── mailbox property-tag buffer layout ─── */

struct prop_buffer {
	uint32_t size;   /* total buffer size in bytes */
	uint32_t code;   /* 0 = request, 0x80000000 = success, 0x80000001 = failure */
	/* tags follow here, then a u32 end-tag (PROPTAG_END) */
};

#define PROP_CODE_REQUEST          0x00000000u
#define PROP_CODE_RESPONSE_SUCCESS 0x80000000u

/* ─── helpers ─── */

static uint32_t align_up(uint32_t value, uint32_t align) {
	return (value + align - 1) & (~(align - 1));
}

/*
 * mailbox_get_tags — send a block of property tags to the firmware and
 * retrieve responses.  Uses the noncached alias first, then falls back
 * to coherent.  Mirrors Circle's CBcmPropertyTags::GetTags().
 *
 * On success returns 0; the caller reads responses from the same
 * in/out buffer.
 */
static int mailbox_get_tags(void *tags, uint32_t tags_size) {
	uint32_t buf_size = sizeof(struct prop_buffer) + tags_size + sizeof(uint32_t);
	uint32_t *shadow = NULL;
	int result = -1;

	struct prop_buffer *buf = (struct prop_buffer *)dma_alloc(0, buf_size);
	if (buf == NULL) {
		return -1;
	}

	/* Build request */
	buf->size = buf_size;
	buf->code = PROP_CODE_REQUEST;
	memcpy((uint8_t *)buf + sizeof(struct prop_buffer), tags, tags_size);
	*(uint32_t *)((uint8_t *)buf + sizeof(struct prop_buffer) + tags_size) = PROPTAG_END;

	/* Prepare shadow for alias fallback from the fully built request. */
	shadow = (uint32_t *)dma_alloc(0, buf_size);
	if (shadow != NULL) {
		memcpy(shadow, buf, buf_size);
	}

	mail_message_t msg;
	memset(&msg, 0, sizeof(msg));
	msg.data = ((uint32_t)dma_phy_addr(0, (ewokos_addr_t)buf)
			| MAILBOX_VC_ALIAS_NONCACHED) >> 4;
	msg.channel = PROPERTY_CHANNEL;

	if (bcm2712_mailbox_call_timeout(&msg, 0) == 0
			&& buf->code == PROP_CODE_RESPONSE_SUCCESS) {
		result = 0;
		goto copy_and_out;
	}

	/* Fallback: try coherent alias with original buffer content */
	if (shadow != NULL) {
		memcpy(buf, shadow, buf_size);
	} else {
		goto out;
	}

	memset(&msg, 0, sizeof(msg));
	msg.data = ((uint32_t)dma_phy_addr(0, (ewokos_addr_t)buf)
			| MAILBOX_VC_ALIAS_COHERENT) >> 4;
	msg.channel = PROPERTY_CHANNEL;

	if (bcm2712_mailbox_call_timeout(&msg, 0) == 0
			&& buf->code == PROP_CODE_RESPONSE_SUCCESS) {
		result = 0;
		goto copy_and_out;
	}

copy_and_out:
	if (result == 0) {
		memcpy(tags, (uint8_t *)buf + sizeof(struct prop_buffer), tags_size);
	}
out:
	if (shadow) {
		dma_free(0, (ewokos_addr_t)shadow);
	}
	dma_free(0, (ewokos_addr_t)buf);
	return result;
}

/* ─── framebuffer validation & adoption ─── */

/*
 * Infer the actual bits-per-pixel from pitch and width when the firmware
 * echoes back a SET_DEPTH value that doesn't match the buffer it allocated.
 * The Pi 5 firmware often refuses to change the boot FB depth but still
 * passes through the requested value in the SET_DEPTH response tag.
 */
static uint32_t fb_depth_from_pitch(uint32_t width, uint32_t pitch) {
	if (width == 0)
		return 0;
	uint32_t bpp = (pitch * 8) / width;
	if (bpp == 16 || bpp == 32)
		return bpp;
	return 0;
}

static int fb_adopt(const sys_info_t *sysinfo,
		uint32_t w,  uint32_t h,
		uint32_t vw, uint32_t vh, uint32_t dep,
		ewokos_addr_t phy, uint32_t size, uint32_t pitch,
		fbinfo_t *info) {
	if ((w == 0) || (h == 0) || (phy == 0) || (size == 0)) {
		klog("fb: adopt bad base w=%u h=%u phy=%x size=%u\n", w, h, phy, size);
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
	info->pointer = sysinfo->sys_dma.v_base + sysinfo->sys_dma.size;
	info->size = size;
	info->xoffset = 0;
	info->yoffset = 0;
	info->size_max = align_up(size, sysinfo->page_size == 0 ? 4096 : sysinfo->page_size);
	info->dma_id = -1;

	if (syscall3(SYS_MEM_MAP,
			(ewokos_addr_t)info->pointer,
			(ewokos_addr_t)info->phy_base,
			(ewokos_addr_t)info->size_max) == 0) {
		klog("fb: mem_map fail v=%x phy=%x size=%u\n",
				info->pointer, info->phy_base, info->size_max);
		memset(info, 0, sizeof(fbinfo_t));
		return -1;
	}
	return 0;
}

/* ─── property-tag helpers ─── */

static void fb_init_tags_init(fb_init_tags_t *t,
		uint32_t w, uint32_t h, uint32_t dep) {
	memset(t, 0, sizeof(*t));

	/* SET_DISPLAY_NUM */
	t->set_display_num.tag.tag_id        = PROPTAG_SET_DISPLAY_NUM;
	t->set_display_num.tag.value_buf_size = 4;
	t->set_display_num.tag.value_length  = 4;
	t->set_display_num.value             = 0;

	/* SET_PHYS_WIDTH_HEIGHT */
	t->set_phys.tag.tag_id        = PROPTAG_SET_PHYS_WIDTH_HEIGHT;
	t->set_phys.tag.value_buf_size = 8;
	t->set_phys.tag.value_length  = 8;
	t->set_phys.width             = w;
	t->set_phys.height            = h;

	/* SET_VIRT_WIDTH_HEIGHT */
	t->set_virt.tag.tag_id        = PROPTAG_SET_VIRT_WIDTH_HEIGHT;
	t->set_virt.tag.value_buf_size = 8;
	t->set_virt.tag.value_length  = 8;
	t->set_virt.width             = w;
	t->set_virt.height            = h;

	/* SET_DEPTH */
	t->set_depth.tag.tag_id        = PROPTAG_SET_DEPTH;
	t->set_depth.tag.value_buf_size = 4;
	t->set_depth.tag.value_length  = 4;
	t->set_depth.value             = dep;

	/* SET_PIXEL_ORDER */
	t->set_pixel_order.tag.tag_id        = PROPTAG_SET_PIXEL_ORDER;
	t->set_pixel_order.tag.value_buf_size = 4;
	t->set_pixel_order.tag.value_length  = 4;
	t->set_pixel_order.value             = PIXEL_ORDER_BGR;

	/* SET_VIRTUAL_OFFSET */
	t->set_offset.tag.tag_id        = PROPTAG_SET_VIRTUAL_OFFSET;
	t->set_offset.tag.value_buf_size = 8;
	t->set_offset.tag.value_length  = 8;
	t->set_offset.width             = 0;
	t->set_offset.height            = 0;

	/* ALLOCATE_BUFFER */
	t->allocate.tag.tag_id        = PROPTAG_ALLOCATE_BUFFER;
	t->allocate.tag.value_buf_size = 8;
	t->allocate.tag.value_length  = 4;  /* 4 request parameter bytes */
	t->allocate.alignment_or_base = 16;  /* alignment */
	t->allocate.size              = 0;   /* firmware fills in */

	/* GET_PITCH */
	t->get_pitch.tag.tag_id        = PROPTAG_GET_PITCH;
	t->get_pitch.tag.value_buf_size = 4;
	t->get_pitch.tag.value_length  = 0;  /* request */
}

static void fb_query_tags_init(fb_query_tags_t *t) {
	memset(t, 0, sizeof(*t));

	/* SET_DISPLAY_NUM */
	t->set_display_num.tag.tag_id        = PROPTAG_SET_DISPLAY_NUM;
	t->set_display_num.tag.value_buf_size = 4;
	t->set_display_num.tag.value_length  = 4;
	t->set_display_num.value             = 0;

	/* GET_PHYS_WIDTH_HEIGHT */
	t->get_phys.tag.tag_id        = PROPTAG_GET_PHYS_WIDTH_HEIGHT;
	t->get_phys.tag.value_buf_size = 8;
	t->get_phys.tag.value_length  = 0;

	/* GET_VIRT_WIDTH_HEIGHT */
	t->get_virt.tag.tag_id        = PROPTAG_GET_VIRT_WIDTH_HEIGHT;
	t->get_virt.tag.value_buf_size = 8;
	t->get_virt.tag.value_length  = 0;

	/* GET_DEPTH */
	t->get_depth.tag.tag_id        = PROPTAG_GET_DEPTH;
	t->get_depth.tag.value_buf_size = 4;
	t->get_depth.tag.value_length  = 0;

	/* GET_PIXEL_ORDER */
	t->get_pixel_order.tag.tag_id        = PROPTAG_GET_PIXEL_ORDER;
	t->get_pixel_order.tag.value_buf_size = 4;
	t->get_pixel_order.tag.value_length  = 0;

	/* ALLOCATE_BUFFER (0 request bytes = query existing) */
	t->allocate.tag.tag_id        = PROPTAG_ALLOCATE_BUFFER;
	t->allocate.tag.value_buf_size = 8;
	t->allocate.tag.value_length  = 0;
	t->allocate.alignment_or_base = 0;
	t->allocate.size              = 0;

	/* GET_PITCH */
	t->get_pitch.tag.tag_id        = PROPTAG_GET_PITCH;
	t->get_pitch.tag.value_buf_size = 4;
	t->get_pitch.tag.value_length  = 0;
}

static int fb_get_num_displays(uint32_t *count) {
	fb_num_displays_tags_t t;

	memset(&t, 0, sizeof(t));
	t.get_num_displays.tag.tag_id        = PROPTAG_GET_NUM_DISPLAYS;
	t.get_num_displays.tag.value_buf_size = 4;
	t.get_num_displays.tag.value_length  = 0;

	if (mailbox_get_tags(&t, sizeof(t)) != 0) {
		return -1;
	}
	if (!(t.get_num_displays.tag.value_length & VALUE_LENGTH_RESPONSE)) {
		return -1;
	}

	*count = t.get_num_displays.value;
	return 0;
}

static int fb_select_display(uint32_t display_num) {
	prop_tag_simple_t t;

	memset(&t, 0, sizeof(t));
	t.tag.tag_id = PROPTAG_SET_DISPLAY_NUM;
	t.tag.value_buf_size = 4;
	t.tag.value_length = 4;
	t.value = display_num;

	if (mailbox_get_tags(&t, sizeof(t)) != 0) {
		return -1;
	}
	if (!(t.tag.value_length & VALUE_LENGTH_RESPONSE)) {
		return -1;
	}
	return 0;
}

/*
 * fb_get_display_dimensions — query the display's preferred/current
 * resolution. Mirrors Circle's PROPTAG_GET_DISPLAY_DIMENSIONS usage in
 * CBcmFrameBuffer constructor.
 */
static int fb_get_display_dimensions(uint32_t *width, uint32_t *height) {
	fb_dims_tags_t t;

	memset(&t, 0, sizeof(t));
	t.set_display_num.tag.tag_id        = PROPTAG_SET_DISPLAY_NUM;
	t.set_display_num.tag.value_buf_size = 4;
	t.set_display_num.tag.value_length  = 4;
	t.set_display_num.value             = 0;
	t.dims.tag.tag_id        = PROPTAG_GET_DISPLAY_DIMENSIONS;
	t.dims.tag.value_buf_size = 8;
	t.dims.tag.value_length  = 0;

	if (mailbox_get_tags(&t, sizeof(t)) != 0) {
		return -1;
	}

	/* Check the response length bit — firmware should set it */
	if (!(t.dims.tag.value_length & VALUE_LENGTH_RESPONSE)) {
		return -1;
	}

	*width  = t.dims.width;
	*height = t.dims.height;

	if (*width < 640 || *width > 4096 || *height < 480 || *height > 2160) {
		return -1;
	}

	return 0;
}

/* ─── init strategies ─── */

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t depth;
} fb_mode_t;

static int fb_mode_equal(const fb_mode_t *a, const fb_mode_t *b) {
	return a->width == b->width &&
			a->height == b->height &&
			a->depth == b->depth;
}

static int fb_mode_matches_info(const fb_mode_t *mode, const fbinfo_t *info) {
        if (mode == NULL || info == NULL) {
                return 0;
        }
        return info->width == mode->width &&
                        info->height == mode->height;
}

/*
 * fb_try_mode — allocate a new framebuffer via property tags.
 * Uses the structured tag block (SET tags + ALLOCATE).
 */
static int fb_try_mode(const sys_info_t *sysinfo,
		const fb_mode_t *mode, fbinfo_t *info) {
	fb_init_tags_t t;

	fb_init_tags_init(&t, mode->width, mode->height, mode->depth);

	if (mailbox_get_tags(&t, sizeof(t)) != 0) {
		klog("fb: prop call failed %ux%ux%u\n",
				mode->width, mode->height, mode->depth);
		return -1;
	}

	uint32_t w    = t.set_phys.width;
	uint32_t h    = t.set_phys.height;
	uint32_t vw   = t.set_virt.width;
	uint32_t vh   = t.set_virt.height;
	uint32_t dep  = t.set_depth.value;
	/* Mask off VC bus alias bits to get ARM physical address. */
	uint32_t phy  = t.allocate.alignment_or_base & 0x3fffffff;
	uint32_t size = t.allocate.size;
	uint32_t pitch = t.get_pitch.value;

	/*
	 * The Pi 5 firmware often refuses to change the boot FB depth but
	 * echoes the requested value in SET_DEPTH.  Detect the real bpp
	 * from the pitch the firmware DID set on the buffer.
	 */
	uint32_t actual_bpp = fb_depth_from_pitch(w, pitch);
	if (actual_bpp != 0 && actual_bpp != dep) {
		klog("fb: prop depth %u overridden by pitch %u -> %u bpp\n",
				dep, pitch, actual_bpp);
		dep = actual_bpp;
	}

	if (fb_adopt(sysinfo, w, h, vw, vh, dep, phy, size, pitch, info) != 0) {
		klog("fb: alloc rejected %ux%ux%u "
				"w=%u h=%u dep=%u phy=%x size=%u pitch=%u\n",
				mode->width, mode->height, mode->depth,
				w, h, dep, phy, size, pitch);
		return -1;
	}
	return 0;
}

/*
 * fb_query_existing — adopt the framebuffer the firmware already set up at
 * boot.  Uses GET tags plus ALLOCATE_BUFFER with 0 request bytes.
 */
static int fb_query_existing(const sys_info_t *sysinfo, fbinfo_t *info) {
	fb_query_tags_t t;

	fb_query_tags_init(&t);

	if (mailbox_get_tags(&t, sizeof(t)) != 0) {
		klog("fb: query prop call failed\n");
		return -1;
	}

	uint32_t w     = t.get_phys.width;
	uint32_t h     = t.get_phys.height;
	uint32_t vw    = t.get_virt.width;
	uint32_t vh    = t.get_virt.height;
	uint32_t dep   = t.get_depth.value;
	uint32_t phy   = t.allocate.alignment_or_base & 0x3fffffff;
	uint32_t size  = t.allocate.size;
	uint32_t pitch = t.get_pitch.value;

	/*
	 * Detect the real bpp from pitch when the firmware returns a
	 * boot FB whose depth doesn't match what was requested.
	 */
	uint32_t actual_bpp = fb_depth_from_pitch(w, pitch);
	if (actual_bpp != 0 && actual_bpp != dep) {
		klog("fb: query depth %u overridden by pitch %u -> %u bpp\n",
				dep, pitch, actual_bpp);
		dep = actual_bpp;
	}

	if (fb_adopt(sysinfo, w, h, vw, vh, dep, phy, size, pitch, info) != 0) {
		klog("fb: no boot fb w=%u h=%u dep=%u phy=%x size=%u pitch=%u\n",
				w, h, dep, phy, size, pitch);
		return -1;
	}
	return 0;
}

/*
 * fb_channel1_init — legacy channel-1 simple framebuffer message.
 * Predates the property interface; the firmware fills in the response
 * fields in place.
 */
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t vwidth;
	uint32_t vheight;
	uint32_t pitch;
	uint32_t depth;
	uint32_t x;
	uint32_t y;
	uint32_t pointer;
	uint32_t size;
} __attribute__((aligned(16))) fb_ch1_msg_t;

static int fb_channel1_init(const sys_info_t *sysinfo,
		const fb_mode_t *mode, fbinfo_t *info) {
	fb_ch1_msg_t *msg = (fb_ch1_msg_t *)dma_alloc(0, sizeof(fb_ch1_msg_t));
	mail_message_t mailbox_msg;
	uint32_t w, h, vw, vh, dep, phy, size, pitch;

	if (msg == NULL) {
		return -1;
	}

	memset(msg, 0, sizeof(fb_ch1_msg_t));
	msg->width = mode->width;
	msg->height = mode->height;
	msg->vwidth = mode->width;
	msg->vheight = mode->height;
	msg->depth = mode->depth;

	memset(&mailbox_msg, 0, sizeof(mailbox_msg));
	mailbox_msg.data = ((uint32_t)dma_phy_addr(0, (ewokos_addr_t)msg)
			| MAILBOX_VC_ALIAS_NONCACHED) >> 4;
	mailbox_msg.channel = FRAMEBUFFER_CHANNEL;

	if (bcm2712_mailbox_call_timeout(&mailbox_msg, 0) != 0) {
		klog("fb: ch1 timeout %ux%ux%u\n",
				mode->width, mode->height, mode->depth);
		dma_free(0, (ewokos_addr_t)msg);
		return -1;
	}

	w = msg->width;
	h = msg->height;
	vw = msg->vwidth;
	vh = msg->vheight;
	dep = msg->depth;
	phy = msg->pointer & 0x3fffffff;
	size = msg->size;
	pitch = msg->pitch;

	/*
	 * The channel-1 response also carries a depth that may differ from
	 * what the pitch implies. Trust the pitch.
	 */
	uint32_t actual_bpp = fb_depth_from_pitch(w, pitch);
	if (actual_bpp != 0 && actual_bpp != dep) {
		klog("fb: ch1 depth %u overridden by pitch %u -> %u bpp\n",
				dep, pitch, actual_bpp);
		dep = actual_bpp;
	}

	dma_free(0, (ewokos_addr_t)msg);

	if (fb_adopt(sysinfo, w, h, vw, vh, dep, phy, size, pitch, info) != 0) {
		klog("fb: ch1 rejected %ux%ux%u w=%u h=%u dep=%u phy=%x size=%u pitch=%u\n",
				mode->width, mode->height, mode->depth,
				w, h, dep, phy, size, pitch);
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
	fb_mode_t requested;
	uint32_t displays = 0;
        int strict_mode = (w != 0 && h != 0);
	fb_mode_t fallbacks[] = {
		{1024, 768, 32},
		{800,  600, 32},
		{640,  480, 32},
		{640,  480, 16},
	};
	const uint32_t n_fallbacks = sizeof(fallbacks) / sizeof(fallbacks[0]);

	memset(&_fb_info, 0, sizeof(fbinfo_t));
	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);

	if (bcm2712_mailbox_init() == 0) {
		klog("fb_init: mmio map failed\n");
		return -1;
	}

	if (fb_get_num_displays(&displays) == 0) {
		klog("fb_init: displays=%u\n", displays);
	}
	if (fb_select_display(0) != 0) {
		klog("fb_init: select display0 failed\n");
	}

	/* Validate / auto-detect resolution */
	if (dep != 16 && dep != 32) {
		dep = 32;
	}

	if (w == 0 || h == 0) {
		uint32_t det_w = 0, det_h = 0;
		if (fb_get_display_dimensions(&det_w, &det_h) == 0) {
			w = det_w;
			h = det_h;
			klog("fb_init: detected display %ux%u\n", w, h);
		} else {
			w = (w == 0) ? 1024 : w;
			h = (h == 0) ? 768  : h;
		}
	}

	requested.width  = w;
	requested.height = h;
	requested.depth  = dep;

	klog("fb_init: requesting %ux%ux%u\n", w, h, dep);

	if (strict_mode && bcm2712_native_hdmi_supported(w, h, dep)) {
		klog("fb_init: trying native hdmi0 path\n");
		if (bcm2712_native_hdmi_init(&sysinfo, w, h, dep, &_fb_info) == 0) {
			goto done;
		}
		klog("fb_init: native hdmi0 path failed\n");
	}

	/* Strategy 1: property tags — allocate new framebuffer */
        if (fb_try_mode_list(&sysinfo, &requested,
                        strict_mode ? NULL : fallbacks,
                        strict_mode ? 0 : n_fallbacks,
			fb_try_mode, "prop", &_fb_info) == 0) {
		goto done;
	}

	/* Strategy 2: adopt the firmware's existing boot framebuffer */
	klog("fb_init: trying boot fb query\n");
	if (fb_query_existing(&sysinfo, &_fb_info) == 0) {
                if (!strict_mode || fb_mode_matches_info(&requested, &_fb_info)) {
                        goto done;
                }
                klog("fb_init: reject boot fb %ux%u for strict request %ux%u\n",
                                _fb_info.width, _fb_info.height,
                                requested.width, requested.height);
                memset(&_fb_info, 0, sizeof(_fb_info));
        }

        /* Strategy 3: legacy channel-1 framebuffer message */
        klog("fb_init: trying channel 1\n");
        if (fb_try_mode_list(&sysinfo, &requested,
                        strict_mode ? NULL : fallbacks,
                        strict_mode ? 0 : n_fallbacks,
                        fb_channel1_init, "ch1", &_fb_info) == 0) {
                if (!strict_mode || fb_mode_matches_info(&requested, &_fb_info)) {
                        goto done;
                }
                klog("fb_init: reject ch1 fb %ux%u for strict request %ux%u\n",
                                _fb_info.width, _fb_info.height,
                                requested.width, requested.height);
                memset(&_fb_info, 0, sizeof(_fb_info));
        }

        klog("fb_init: all modes failed\n");
        return -1;

done:
        klog("fb_init: %ux%u@%u pitch=%u phy=%x size=%u\n",
                        _fb_info.width, _fb_info.height, _fb_info.depth,
                        _fb_info.pitch, _fb_info.phy_base, _fb_info.size);
        return 0;
}

fbinfo_t *bcm2712_get_fbinfo(void) {
        return &_fb_info;
}
