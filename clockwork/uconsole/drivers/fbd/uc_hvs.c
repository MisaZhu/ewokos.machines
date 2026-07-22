#include "uc_hvs.h"
#include "uc_time.h"

#include <stdint.h>

#include <ewoksys/mmio.h>

/* ---------- HVS register offsets (from Linux drivers/gpu/drm/vc4). --- */

#define SCALER_DISPCTRL           0x00000000U
#define  SCALER_DISPCTRL_ENABLE     (1U << 31)

/*
 * Global status: latches AXI read-response errors and per-channel
 * underflow events regardless of the IRQ enables (W1C).
 */
#define SCALER_DISPSTAT           0x00000004U
#define  SCALER_DISPSTAT_RESP_MASK  (0x3U << 14)   /* 0 = OKAY */
#define  SCALER_DISPSTAT_DMA_ERROR  (1U << 7)
#define  SCALER_DISPSTAT_EUFLOW1    (1U << (9 + 8))  /* channel-1 underflow */

/*
 * DSP3_MUX = bits [19:18] of SCALER_DISPCTRL.  On BCM2711 (HVS5) this
 * selects which of HVS channels 0/1/2 drives PV1 (i.e. DSI1).  A value
 * of 3 leaves PV1 disconnected.  Upstream vc4_hvs_bind() defaults this
 * to 2; we route our channel-1 dlist to PV1 by setting it to 1.
 */
#define  SCALER_DISPCTRL_DSP3_MUX_MASK   (0x3U << 18)
#define  SCALER_DISPCTRL_DSP3_MUX_SHIFT  18

#define SCALER_DISPLIST0          0x00000020U
#define SCALER_DISPLIST1          0x00000024U
#define SCALER_DISPLIST2          0x00000028U

#define SCALER_DISPCTRL0          0x00000040U
#define SCALER_DISPBKGND0         0x00000044U
#define SCALER_DISPSTAT0          0x00000048U
#define SCALER_DISPBASE0          0x0000004cU

#define SCALER_DISPCTRL1          0x00000050U
#define SCALER_DISPBKGND1         0x00000054U
#define SCALER_DISPSTAT1          0x00000058U
#define SCALER_DISPBASE1          0x0000005cU

#define SCALER_DISPCTRL2          0x00000060U
#define SCALER_DISPBKGND2         0x00000064U
#define SCALER_DISPSTAT2          0x00000068U
#define SCALER_DISPBASE2          0x0000006cU

#define  SCALER_DISPCTRLX_ENABLE    (1U << 31)
#define  SCALER_DISPCTRLX_RESET     (1U << 30)

/* DISPSTATX fields (same layout on HVS4/HVS5). */
#define  SCALER_DISPSTATX_MODE_SHIFT      30      /* bits 31:30 */
#define  SCALER_DISPSTATX_MODE_MASK       (0x3U << 30)
#define   SCALER_DISPSTATX_MODE_DISABLED  0U
#define   SCALER_DISPSTATX_MODE_INIT      1U
#define   SCALER_DISPSTATX_MODE_RUN       2U
#define   SCALER_DISPSTATX_MODE_EOF       3U
#define  SCALER_DISPSTATX_FRAME_COUNT_SHIFT 12    /* bits 17:12 */
#define  SCALER_DISPSTATX_FRAME_COUNT_MASK  (0x3fU << 12)

/* HVS5 (BCM2711) layout of DISPCTRLX. */
#define  SCALER5_DISPCTRLX_WIDTH_SHIFT   16      /* bits 28:16 */
#define  SCALER5_DISPCTRLX_HEIGHT_SHIFT  0       /* bits 12:0  */

#define  SCALER_DISPBKGND_FILL     (1U << 24)

/*
 * BIT(31): auto-generate HSTART toward the HVS pipeline on each PV
 * hstart.  Without this the plane composition never advances -- the
 * HVS just sits on the dlist forever and no pixels reach PV1.
 */
#define  SCALER_DISPBKGND_AUTOHS   (1U << 31)

/* HVS5 dlist RAM starts at offset 0x4000, entries are 32-bit words. */
#define SCALER5_DLIST_START       0x00004000U

/*
 * Upstream reserves the first 32 dlist words (0x4000..0x407f) for
 * firmware boot-time setup (HVS_BOOTLOADER_DLIST_END). Our dlist must
 * start past them or we clobber / depend on undefined firmware state.
 */
#define HVS_BOOTLOADER_DLIST_END  32U

/*
 * COB (Composite Output Buffer) allocation, matching upstream
 * vc4_hvs_bind() for VC4_GEN_5.  Firmware boot with vc4-kms leaves
 * SCALER_DISPBASEX at reset value 0, i.e. every channel gets a
 * zero-size output buffer and no pixels ever come out.
 *   DISPBASE2: base 0x0000 top 0x3000
 *   DISPBASE1: base 0x3010 top 0x6010
 *   DISPBASE0: base 0x6020 top 0xAD80
 */
#define UC_HVS5_DISPBASE2         0x30000000U
#define UC_HVS5_DISPBASE1         0x60103010U
#define UC_HVS5_DISPBASE0         0xAD806020U

/* ---------- DLIST word bit definitions. --------- */

#define SCALER_CTL0_END                (1U << 31)
#define SCALER_CTL0_VALID              (1U << 30)
#define SCALER_CTL0_SIZE_SHIFT         24        /* bits 29:24 */

#define SCALER_CTL0_ORDER_SHIFT        13
#define  HVS_PIXEL_ORDER_ARGB          2U

#define SCALER5_CTL0_UNITY             (1U << 15)
#define SCALER5_CTL0_ALPHA_EXPAND      (1U << 12)
#define SCALER5_CTL0_RGB_EXPAND        (1U << 11)

#define SCALER_CTL0_PIXEL_FORMAT_SHIFT 0
#define  HVS_PIXEL_FORMAT_RGB565       4U
#define  HVS_PIXEL_FORMAT_RGBA8888     7U

#define SCALER5_POS0_START_Y_SHIFT     16
#define SCALER5_POS0_START_X_SHIFT     0

#define SCALER5_CTL2_ALPHA_MODE_FIXED  (1U << 30)
#define SCALER5_CTL2_ALPHA_SHIFT       4         /* opaque = 0xfff */

#define SCALER5_POS2_HEIGHT_SHIFT      16
#define SCALER5_POS2_WIDTH_SHIFT       0

static volatile uint32_t* _hvs = 0;

/* dlist layout bookkeeping for the runtime plane probe. */
static uint32_t _dl_base = 0;      /* byte offset of our dlist in HVS RAM */
#define UC_DL_CTL0_IDX   0U
#define UC_DL_CTX_IDX    4U
#define UC_DL_PTR0_IDX   5U
#define UC_DL_PTRCTX_IDX 6U

static void _hvs_init(void) {
	if (_hvs == 0 && _mmio_base != 0) {
		_hvs = (volatile uint32_t*)(uintptr_t)(_mmio_base + UC_HVS_OFFSET);
	}
}

static uint32_t _hvs_read(uint32_t off) {
	if (_hvs == 0) return 0;
	return _hvs[off / 4];
}

static void _hvs_write(uint32_t off, uint32_t v) {
	if (_hvs == 0) return;
	_hvs[off / 4] = v;
}

uint32_t uc_hvs_read_raw(uint32_t off) {
	_hvs_init();
	return _hvs_read(off);
}

/*
 * Build an HVS5 display-list for one full-screen unity plane and drop
 * it into channel 1's dlist SRAM.  Layout (8 payload words + terminator):
 *
 *  0  CTL0     valid|unity|alpha_expand|rgb_expand|order|format|size
 *  1  POS0     y<<16 | x<<0
 *  2  CTL2     alpha_mode | alpha
 *  3  POS2     height<<16 | width<<0
 *  4  CTX0     0xC0C0C0C0 (scratch, written by HVS)
 *  5  PTR0     physical framebuffer address (bus address on BCM2711)
 *  6  CTX_PTR  0xC0C0C0C0
 *  7  PITCH0   bytes per row
 *  8  0x80000000  end marker
 */
static uint32_t _write_dlist(uint32_t phy_fb, uint32_t w, uint32_t h, uint32_t dep) {
	uint32_t base = SCALER5_DLIST_START + HVS_BOOTLOADER_DLIST_END * 4U;
	uint32_t hvs_fmt;
	uint32_t hvs_order = HVS_PIXEL_ORDER_ARGB;
	uint32_t pitch;
	uint32_t ctl0;
	uint32_t size_words = 8;
	uint32_t idx = 0;

	_dl_base = base;

	if (dep == 16) {
		hvs_fmt = HVS_PIXEL_FORMAT_RGB565;
		pitch = w * 2U;
	} else {
		hvs_fmt = HVS_PIXEL_FORMAT_RGBA8888;
		pitch = w * 4U;
	}

	ctl0 = SCALER_CTL0_VALID |
	       SCALER5_CTL0_UNITY |
	       SCALER5_CTL0_ALPHA_EXPAND |
	       SCALER5_CTL0_RGB_EXPAND |
	       (hvs_order << SCALER_CTL0_ORDER_SHIFT) |
	       (hvs_fmt   << SCALER_CTL0_PIXEL_FORMAT_SHIFT) |
	       (size_words << SCALER_CTL0_SIZE_SHIFT);

	_hvs_write(base + (idx++) * 4U, ctl0);
	/* POS0: x=0, y=0. */
	_hvs_write(base + (idx++) * 4U, 0);
	/* CTL2: fixed alpha = 0xfff (opaque). */
	_hvs_write(base + (idx++) * 4U,
			SCALER5_CTL2_ALPHA_MODE_FIXED |
			(0xfffU << SCALER5_CTL2_ALPHA_SHIFT));
	/* POS2: source size. */
	_hvs_write(base + (idx++) * 4U,
			(h << SCALER5_POS2_HEIGHT_SHIFT) |
			(w << SCALER5_POS2_WIDTH_SHIFT));
	/* Context slot for POS. */
	_hvs_write(base + (idx++) * 4U, 0xC0C0C0C0U);
	/*
	 * PTR0: framebuffer address AS SEEN BY THE HVS.  The HVS sits on
	 * the BCM2711 "soc" bus whose dma-ranges is
	 *   <0xc0000000  0x0 0x00000000  0x40000000>
	 * i.e. legacy masters see the first 1GB of SDRAM at bus address
	 * 0xC0000000.  Linux hands vc4 a dma_addr_t that already carries
	 * this offset; writing the raw ARM physical address makes the
	 * HVS fetch from the wrong place and nothing is displayed.
	 */
	_hvs_write(base + (idx++) * 4U, phy_fb | 0xC0000000U);
	/* Context slot for PTR. */
	_hvs_write(base + (idx++) * 4U, 0xC0C0C0C0U);
	/* PITCH0. */
	_hvs_write(base + (idx++) * 4U, pitch);
	/* End of dlist. */
	_hvs_write(base + (idx++) * 4U, SCALER_CTL0_END);

	return HVS_BOOTLOADER_DLIST_END;  /* dlist starts past the fw-reserved words */
}

int uc_hvs_bringup(uint32_t phy_fb, uint32_t w, uint32_t h, uint32_t dep) {
	uint32_t dlist_word_idx;
	uint32_t dispctrl;

	_hvs_init();
	if (_hvs == 0) return -1;

	/*
	 * Global enable + route HVS channel 1 to PV1 (DSI1).
	 *
	 * Upstream vc4_hvs_bind() defaults DSP3_MUX to 2 because it uses
	 * channel 2 for DSI1.  We use channel 1, so DSP3_MUX must be 1 or
	 * PV1 is left disconnected from anything HVS is composing.
	 */
	dispctrl = _hvs_read(SCALER_DISPCTRL);
	dispctrl |= SCALER_DISPCTRL_ENABLE;
	dispctrl &= ~SCALER_DISPCTRL_DSP3_MUX_MASK;
	dispctrl |= (1U << SCALER_DISPCTRL_DSP3_MUX_SHIFT);
	_hvs_write(SCALER_DISPCTRL, dispctrl);

	/*
	 * Program the COB allocation (upstream vc4_hvs_bind, VC4_GEN_5).
	 * Reset value 0 means zero-size output buffers on every channel,
	 * so without this no pixels ever leave the HVS.
	 */
	_hvs_write(SCALER_DISPBASE2, UC_HVS5_DISPBASE2);
	_hvs_write(SCALER_DISPBASE1, UC_HVS5_DISPBASE1);
	_hvs_write(SCALER_DISPBASE0, UC_HVS5_DISPBASE0);

	/* Reset channel 1 (write 0, RESET, 0 -- matches vc4_hvs_init_channel). */
	_hvs_write(SCALER_DISPCTRL1, 0);
	_hvs_write(SCALER_DISPCTRL1, SCALER_DISPCTRLX_RESET);
	uc_udelay(10);
	_hvs_write(SCALER_DISPCTRL1, 0);

	/* Write dlist -> get word index (in the 32-bit dlist RAM). */
	dlist_word_idx = _write_dlist(phy_fb, w, h, dep);

	/* Point channel 1 at the dlist start (dlist RAM word index). */
	_hvs_write(SCALER_DISPLIST1, dlist_word_idx);

	/*
	 * Enable channel 1 with panel size (HVS5 layout).  This must be
	 * written BEFORE DISPBKGND on upstream, so preserve that order.
	 */
	_hvs_write(SCALER_DISPCTRL1,
			SCALER_DISPCTRLX_ENABLE |
			(w << SCALER5_DISPCTRLX_WIDTH_SHIFT) |
			(h << SCALER5_DISPCTRLX_HEIGHT_SHIFT));

	/*
	 * Background fill + AUTOHS.  AUTOHS (BIT 31) is what tells HVS to
	 * advance the dlist on each PV hstart -- without it, the pipeline
	 * never actually moves and PV1 gets no pixels.
	 *
	 * Background colour is deliberately WHITE (0x00ffffff): if the
	 * plane/dlist is rejected by the HVS, the channel still outputs
	 * the background.  White-instead-of-black on the panel therefore
	 * proves the HVS->PV1->DSI1->panel path end to end even when the
	 * plane fetch is broken; a valid plane covers the whole screen
	 * so the white never shows in the good case.
	 */
	_hvs_write(SCALER_DISPBKGND1,
			SCALER_DISPBKGND_AUTOHS |
			SCALER_DISPBKGND_FILL |
			0x00ffffffU);

	uc_udelay(100);
	return 0;
}

/*
 * Runtime liveness probes for the blink-code trace.  Only meaningful
 * AFTER the PV has been enabled and DSI switched to video mode: the
 * channel sits in INIT until the PV sends its first vstart.
 */
int uc_hvs_channel_running(void) {
	uint32_t mode;

	_hvs_init();
	if (_hvs == 0) return -1;
	mode = (_hvs_read(SCALER_DISPSTAT1) & SCALER_DISPSTATX_MODE_MASK)
			>> SCALER_DISPSTATX_MODE_SHIFT;
	return (mode == SCALER_DISPSTATX_MODE_RUN ||
		mode == SCALER_DISPSTATX_MODE_EOF) ? 0 : -1;
}

int uc_hvs_frames_advancing(uint32_t wait_ms) {
	uint32_t c0;
	uint32_t c1;

	_hvs_init();
	if (_hvs == 0) return -1;
	c0 = (_hvs_read(SCALER_DISPSTAT1) & SCALER_DISPSTATX_FRAME_COUNT_MASK)
			>> SCALER_DISPSTATX_FRAME_COUNT_SHIFT;
	uc_mdelay(wait_ms);
	c1 = (_hvs_read(SCALER_DISPSTAT1) & SCALER_DISPSTATX_FRAME_COUNT_MASK)
			>> SCALER_DISPSTATX_FRAME_COUNT_SHIFT;
	return (c1 != c0) ? 0 : -1;
}

/*
 * Runtime plane-fetch probe.  The HVS re-parses the dlist every frame
 * and REWRITES the two context words (we pre-filled 0xC0C0C0C0), and
 * the global DISPSTAT latches AXI read-response errors and channel
 * underflow independent of any IRQ enables.  Together they separate
 * "plane never processed" from "plane processed but fetch failing"
 * without any log access:
 *   returns 1 = context words untouched: HVS is SKIPPING our plane
 *               (dlist rejected) and only outputs the background
 *   returns 2 = AXI read error latched: plane processed but the
 *               framebuffer FETCH fails (bad bus address)
 *   returns 3 = channel-1 underflow latched: fetch too slow
 *   returns 0 = plane processed, fetch clean -- the white on the
 *               glass is CONTENT, not the background fill
 */
int uc_hvs_plane_probe(void) {
	uint32_t stat;

	_hvs_init();
	if (_hvs == 0 || _dl_base == 0) return -1;

	/* Clear latched status (W1C), then let a few frames pass. */
	_hvs_write(SCALER_DISPSTAT, _hvs_read(SCALER_DISPSTAT));
	uc_mdelay(100);

	if (_hvs_read(_dl_base + UC_DL_CTX_IDX * 4U) == 0xC0C0C0C0U &&
	    _hvs_read(_dl_base + UC_DL_PTRCTX_IDX * 4U) == 0xC0C0C0C0U) {
		return 1;
	}

	stat = _hvs_read(SCALER_DISPSTAT);
	if ((stat & SCALER_DISPSTAT_RESP_MASK) != 0 ||
	    (stat & SCALER_DISPSTAT_DMA_ERROR) != 0) {
		return 2;
	}
	if ((stat & SCALER_DISPSTAT_EUFLOW1) != 0) {
		return 3;
	}
	return 0;
}

/*
 * Hot-swap PTR0 in the live dlist (re-read each frame), for the
 * bus-alias A/B trial.  Also refresh the context words so a later
 * probe reflects the NEW address.
 */
void uc_hvs_set_ptr0(uint32_t bus_addr) {
	_hvs_init();
	if (_hvs == 0 || _dl_base == 0) return;
	_hvs_write(_dl_base + UC_DL_PTR0_IDX * 4U, bus_addr);
	_hvs_write(_dl_base + UC_DL_CTX_IDX * 4U, 0xC0C0C0C0U);
	_hvs_write(_dl_base + UC_DL_PTRCTX_IDX * 4U, 0xC0C0C0C0U);
}
