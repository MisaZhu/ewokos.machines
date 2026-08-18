#include "dsi1_internal.h"

#include <stdint.h>
#include <stdio.h>

#include <ewoksys/mmio.h>

/*
 * HVS (Hardware Video Scaler) at MMIO+0x400000 on both gens.
 *
 * PV<->FIFO routing (vc4_crtc.c / vc4_hvs.c):
 *   gen4: PV0 hardwired to FIFO 0, PV2 to FIFO 1 — but PV1 (the DSI1
 *   pixelvalve) is the "DSP3" output and DSP3_MUX (bits 19:18)
 *   selects the FIFO that feeds it; upstream vc4_hvs_bind routes it
 *   to FIFO 2 ("Set DSP3 (PV1) to use HVS channel 2").  Parking the
 *   mux at 3 disconnects PV1 from every FIFO: the PV free-runs while
 *   the enabled channel never sees a vstart (mode stays DISABLED) —
 *   proven on a Pi 3A+.
 *   gen5: PV1 is HVS output 3 and DSP3_MUX selects the FIFO that
 *   feeds it (3 = disconnected).
 * So the scan-out channel is: port0 -> 0; port1 -> 2 on gen4, 1 on
 * gen5 (+DSP3_MUX).  The dlist word layout and the COB allocation
 * differ between HVS4 (BCM2835/2837, gen4) and HVS5 (BCM2711, gen5);
 * everything else is shared.
 */

#define SCALER_DISPCTRL           0x00000000U
#define  SCALER_DISPCTRL_ENABLE     (1U << 31)
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
#define SCALER_DISPBASE2          0x0000006cU

#define  SCALER_DISPCTRLX_ENABLE    (1U << 31)
#define  SCALER_DISPCTRLX_RESET     (1U << 30)

/* HVS4 (gen4) DISPCTRLX size fields. */
#define  SCALER_DISPCTRLX_WIDTH_SHIFT    12   /* bits 23:12 */
#define  SCALER_DISPCTRLX_HEIGHT_SHIFT   0    /* bits 11:0  */
/* HVS5 (gen5) DISPCTRLX size fields. */
#define  SCALER5_DISPCTRLX_WIDTH_SHIFT   16   /* bits 28:16 */
#define  SCALER5_DISPCTRLX_HEIGHT_SHIFT  0    /* bits 12:0  */

/* DISPSTATX channel mode (bits 31:30, same layout both gens). */
#define  SCALER_DISPSTATX_MODE_SHIFT      30
#define  SCALER_DISPSTATX_MODE_MASK       (0x3U << 30)
#define   SCALER_DISPSTATX_MODE_RUN       2U
#define   SCALER_DISPSTATX_MODE_EOF       3U

/* Frame counters for channels 0 and 1 BOTH live in DISPSTAT1, at
 * different bit positions per generation (vc4_regs.h); channel 2's
 * counter lives in DISPSTAT2. */
#define  SCALER_DISPSTAT1_FRCNT0_SHIFT     18   /* gen4: bits 23:18 */
#define  SCALER_DISPSTAT1_FRCNT0_MASK      (0x3fU << 18)
#define  SCALER_DISPSTAT1_FRCNT1_SHIFT     12   /* gen4: bits 17:12 */
#define  SCALER_DISPSTAT1_FRCNT1_MASK      (0x3fU << 12)
#define  SCALER5_DISPSTAT1_FRCNT0_SHIFT    20   /* gen5: bits 25:20 */
#define  SCALER5_DISPSTAT1_FRCNT0_MASK     (0x3fU << 20)
#define  SCALER5_DISPSTAT1_FRCNT1_SHIFT    14   /* gen5: bits 19:14 */
#define  SCALER5_DISPSTAT1_FRCNT1_MASK     (0x3fU << 14)
#define SCALER_DISPSTAT2          0x00000068U
#define  SCALER_DISPSTAT2_FRCNT2_SHIFT     12   /* gen4: bits 17:12 */
#define  SCALER_DISPSTAT2_FRCNT2_MASK      (0x3fU << 12)
#define  SCALER5_DISPSTAT2_FRCNT2_SHIFT    14   /* gen5: bits 19:14 */
#define  SCALER5_DISPSTAT2_FRCNT2_MASK     (0x3fU << 14)

/* DISPBKGND bits.  Bit 31 is AUTOHS on HVS4 but BCK2BCK on HVS5,
 * so it is only set on gen4 (vc4_hvs.c vc4_crtc setup). */
#define  SCALER_DISPBKGND_AUTOHS   (1U << 31)
#define  SCALER5_DISPBKGND_BCK2BCK (1U << 31)
#define  SCALER_DISPBKGND_FILL     (1U << 24)

/* dlist SRAM base differs per generation (vc4_regs.h / vc4_hvs.c bind):
 * gen4 SCALER_DLIST_START = 0x2000, gen5 SCALER5_DLIST_START = 0x4000.
 * Upstream reserves the first 32 words for firmware boot-time setup
 * (HVS_BOOTLOADER_DLIST_END). */
#define SCALER4_DLIST_START       0x00002000U
#define SCALER5_DLIST_START       0x00004000U
#define HVS_BOOTLOADER_DLIST_END  32U

/*
 * COB (Composite Output Buffer) allocation — upstream vc4_hvs_cob_init.
 * Reset value 0 means zero-size output buffers on every channel, so
 * without this no pixels ever leave the HVS.
 *
 * gen4 (VC4_COB_LINE_WIDTH=2048, 3 lines each, top = size-1):
 *   DISPBASE2 0x17FF0000  DISPBASE1 0x2FFF1800  DISPBASE0 0x50FF3000
 * gen5 (VC5 layout, 16-pixel guard bands):
 *   DISPBASE2 0x30000000  DISPBASE1 0x60103010  DISPBASE0 0xAD806020
 */
#define HVS4_DISPBASE2         0x17FF0000U
#define HVS4_DISPBASE1         0x2FFF1800U
#define HVS4_DISPBASE0         0x50FF3000U
#define HVS5_DISPBASE2         0x30000000U
#define HVS5_DISPBASE1         0x60103010U
#define HVS5_DISPBASE0         0xAD806020U

/* ---------- dlist word bits ---------- */

#define SCALER_CTL0_END                (1U << 31)
#define SCALER_CTL0_VALID              (1U << 30)
#define SCALER_CTL0_SIZE_SHIFT         24        /* bits 29:24 */

#define SCALER_CTL0_ORDER_SHIFT        13
#define  HVS_PIXEL_ORDER_XRGB          2U
#define  HVS_PIXEL_ORDER_ARGB          2U
#define  HVS_PIXEL_ORDER_ABGR          3U

/* gen4 control-word fields. */
#define SCALER_CTL0_RGBA_EXPAND_ROUND  (3U << 11)  /* field 12:11 = 3 */
#define SCALER4_CTL0_UNITY             (1U << 4)
/* gen5 control-word fields. */
#define SCALER5_CTL0_UNITY             (1U << 15)
#define SCALER5_CTL0_ALPHA_EXPAND      (1U << 12)
#define SCALER5_CTL0_RGB_EXPAND        (1U << 11)

#define SCALER_CTL0_PIXEL_FORMAT_SHIFT 0
#define  HVS_PIXEL_FORMAT_RGB565       4U
#define  HVS_PIXEL_FORMAT_RGBA8888     7U

/* gen4 position words. */
#define SCALER4_POS0_FIXED_ALPHA_SHIFT 24
#define SCALER4_POS0_START_Y_SHIFT     12
#define SCALER4_POS0_START_X_SHIFT     0
#define SCALER4_POS2_ALPHA_MODE_FIXED  (1U << 30)
#define SCALER4_POS2_HEIGHT_SHIFT      16
#define SCALER4_POS2_WIDTH_SHIFT       0

/* gen5 position words. */
#define SCALER5_POS0_START_Y_SHIFT     16
#define SCALER5_POS0_START_X_SHIFT     0
#define SCALER5_CTL2_ALPHA_MODE_FIXED  (1U << 30)
#define SCALER5_CTL2_ALPHA_SHIFT       4         /* opaque = 0xfff */
#define SCALER5_POS2_HEIGHT_SHIFT      16
#define SCALER5_POS2_WIDTH_SHIFT       0

/* dlist layout bookkeeping for the plane probe.  The CTX/PTR0/PTRCTX
 * word positions differ per gen (gen4: 3/4/5, gen5: 4/5/6), so their
 * byte offsets are recorded while the dlist is written. */
static uint32_t _dl_base = 0;      /* byte offset of our dlist in HVS RAM */
static uint32_t _dl_ctx_off = 0;
static uint32_t _dl_ptr0_off = 0;
static uint32_t _dl_ptrctx_off = 0;

/* Saved by bringup so the crossbar probe can mirror the channel. */
static uint32_t _w = 0, _h = 0, _dlist_idx = 0;
/* Runtime channel override: the gen4 PV1->FIFO crossbar is assumed
 * hardwired (PV1 reads FIFO 2), but if hardware shows the vstart
 * arriving on another FIFO the scan-out channel follows it. */
static int _channel_override = -1;

/* Scan-out channel: PV0 -> 0; PV1 hardwired to FIFO 2 on gen4,
 * routed through DSP3_MUX to FIFO 1 on gen5. */
static uint32_t _channel(void) {
	if (_channel_override >= 0) {
		return (uint32_t)_channel_override;
	}
	if (!dsi1_port()) {
		return 0U;
	}
	return bcm283x_dsi1_is_gen5() ? 1U : 2U;
}
#define REG_DISPLISTX(ch)  (SCALER_DISPLIST0  + (ch) * 4U)
#define REG_DISPCTRLX(ch)  (SCALER_DISPCTRL0  + (ch) * 0x10U)
#define REG_DISPBKGNDX(ch) (SCALER_DISPBKGND0 + (ch) * 0x10U)
#define REG_DISPSTATX(ch)  (SCALER_DISPSTAT0  + (ch) * 0x10U)

/*
 * Build the dlist for one full-screen unity plane and drop it into
 * channel 1's dlist SRAM.
 *
 * gen4 (7 payload words + terminator):
 *  0  CTL0   valid|rgba_expand_round|order|format|unity
 *  1  POS0   fixed_alpha<<24 | y<<12 | x
 *  2  POS2   alpha_mode_fixed | height<<16 | width
 *  3  CTX    0xC0C0C0C0 (scratch, written by HVS)
 *  4  PTR0   framebuffer bus address
 *  5  CTX    0xC0C0C0C0
 *  6  PITCH0 bytes per row
 *  7  END    0x80000000
 *
 * gen5 (8 payload words + terminator): CTL0 gains alpha/rgb expand +
 * the gen5 unity bit, POS0 loses the fixed alpha, and a CTL2 word
 * (alpha mode + 12-bit alpha) is inserted after POS0.
 *
 * XRGB8888 maps to HVS format RGBA8888 with pixel order ABGR on gen4
 * and ARGB on gen5 (vc4 hvs4_formats/hvs5_formats); RGB565 uses
 * order XRGB on both.
 */
static uint32_t _write_dlist(uint32_t phy_fb, uint32_t w, uint32_t h,
		uint32_t dep, uint32_t pitch, int gen5) {
	uint32_t base = (gen5 ? SCALER5_DLIST_START : SCALER4_DLIST_START) +
			HVS_BOOTLOADER_DLIST_END * 4U;
	uint32_t hvs_fmt;
	uint32_t hvs_order;
	uint32_t ctl0;
	uint32_t size_words;
	uint32_t idx = 0;

	_dl_base = base;

	if (dep == 16) {
		hvs_fmt = HVS_PIXEL_FORMAT_RGB565;
		hvs_order = HVS_PIXEL_ORDER_XRGB;
		if (pitch == 0)
			pitch = w * 2U;
	} else {
		hvs_fmt = HVS_PIXEL_FORMAT_RGBA8888;
		hvs_order = gen5 ? HVS_PIXEL_ORDER_ARGB : HVS_PIXEL_ORDER_ABGR;
		if (pitch == 0)
			pitch = w * 4U;
	}

	if (gen5) {
		size_words = 8;
		ctl0 = SCALER_CTL0_VALID |
		       SCALER5_CTL0_UNITY |
		       SCALER5_CTL0_ALPHA_EXPAND |
		       SCALER5_CTL0_RGB_EXPAND |
		       (hvs_order << SCALER_CTL0_ORDER_SHIFT) |
		       (hvs_fmt   << SCALER_CTL0_PIXEL_FORMAT_SHIFT) |
		       (size_words << SCALER_CTL0_SIZE_SHIFT);
		dsi1_hvs_write(base + (idx++) * 4U, ctl0);
		/* POS0: x=0, y=0. */
		dsi1_hvs_write(base + (idx++) * 4U, 0);
		/* CTL2: fixed alpha = 0xfff (opaque). */
		dsi1_hvs_write(base + (idx++) * 4U,
				SCALER5_CTL2_ALPHA_MODE_FIXED |
				(0xfffU << SCALER5_CTL2_ALPHA_SHIFT));
	} else {
		size_words = 7;
		ctl0 = SCALER_CTL0_VALID |
		       SCALER_CTL0_RGBA_EXPAND_ROUND |
		       (hvs_order << SCALER_CTL0_ORDER_SHIFT) |
		       (hvs_fmt   << SCALER_CTL0_PIXEL_FORMAT_SHIFT) |
		       SCALER4_CTL0_UNITY |
		       (size_words << SCALER_CTL0_SIZE_SHIFT);
		dsi1_hvs_write(base + (idx++) * 4U, ctl0);
		/* POS0: fixed alpha 0xff, x=0, y=0. */
		dsi1_hvs_write(base + (idx++) * 4U,
				(0xffU << SCALER4_POS0_FIXED_ALPHA_SHIFT));
		/* POS2: source size + fixed alpha blend mode. */
		dsi1_hvs_write(base + (idx++) * 4U,
				SCALER4_POS2_ALPHA_MODE_FIXED |
				(h << SCALER4_POS2_HEIGHT_SHIFT) |
				(w << SCALER4_POS2_WIDTH_SHIFT));
	}

	/* POS2 (gen5): source size. */
	if (gen5) {
		dsi1_hvs_write(base + (idx++) * 4U,
				(h << SCALER5_POS2_HEIGHT_SHIFT) |
				(w << SCALER5_POS2_WIDTH_SHIFT));
	}
	/* Context slot for POS. */
	_dl_ctx_off = base + idx * 4U;
	dsi1_hvs_write(base + (idx++) * 4U, 0xC0C0C0C0U);
	/*
	 * PTR0: framebuffer address AS SEEN BY THE HVS.  The HVS sits
	 * on the legacy bus whose dma-ranges is
	 *   <0xc0000000  0x0 0x00000000  0x40000000>
	 * on both gens: the first 1GB of SDRAM is visible at bus
	 * address 0xC0000000.  Writing the raw ARM physical address
	 * makes the HVS fetch from the wrong place.
	 */
	_dl_ptr0_off = base + idx * 4U;
	dsi1_hvs_write(base + (idx++) * 4U, phy_fb | 0xC0000000U);
	/* Context slot for PTR. */
	_dl_ptrctx_off = base + idx * 4U;
	dsi1_hvs_write(base + (idx++) * 4U, 0xC0C0C0C0U);
	/* PITCH0. */
	dsi1_hvs_write(base + (idx++) * 4U, pitch);
	/* End of dlist. */
	dsi1_hvs_write(base + (idx++) * 4U, SCALER_CTL0_END);

	return HVS_BOOTLOADER_DLIST_END;  /* dlist word index in RAM */
}

int bcm283x_dsi1_hvs_bringup(uint32_t phy_fb, uint32_t w, uint32_t h,
		uint32_t dep, uint32_t pitch) {
	int gen5 = bcm283x_dsi1_is_gen5();
	uint32_t ch = _channel();
	uint32_t dlist_word_idx;
	uint32_t dispctrl;
	uint32_t dispbkgnd;

	if (_mmio_base == 0) {
		return -1;
	}

	/*
	 * Global enable + route the HVS channel to the active PV.
	 * gen5: PV1 is HVS output 3, fed through DSP3_MUX; PV0 is
	 * hardwired to FIFO 0 and needs no routing.
	 * gen4: PV1 is the DSP3 output and DSP3_MUX selects its FIFO
	 * — upstream vc4_hvs_bind routes it to FIFO 2 unconditionally
	 * ("Set DSP3 (PV1) to use HVS channel 2").  Mux value 3 means
	 * disconnected: the PV then free-runs while the enabled
	 * channel never receives a vstart (Pi 3A+ symptom).  PV0
	 * (DSI0) is hardwired to FIFO 0, so setting the mux is
	 * harmless there — mirror upstream and always route DSP3 to
	 * the gen4 DSI1 scan-out channel.
	 */
	dispctrl = dsi1_hvs_read(SCALER_DISPCTRL);
	dispctrl |= SCALER_DISPCTRL_ENABLE;
	dispctrl &= ~SCALER_DISPCTRL_DSP3_MUX_MASK;
	if (gen5 && dsi1_port()) {
		dispctrl |= (ch << SCALER_DISPCTRL_DSP3_MUX_SHIFT);
	} else if (!gen5) {
		dispctrl |= (2U << SCALER_DISPCTRL_DSP3_MUX_SHIFT);
	}
	dsi1_hvs_write(SCALER_DISPCTRL, dispctrl);

	/* Program the COB allocation for this generation. */
	if (gen5) {
		dsi1_hvs_write(SCALER_DISPBASE2, HVS5_DISPBASE2);
		dsi1_hvs_write(SCALER_DISPBASE1, HVS5_DISPBASE1);
		dsi1_hvs_write(SCALER_DISPBASE0, HVS5_DISPBASE0);
	} else {
		dsi1_hvs_write(SCALER_DISPBASE2, HVS4_DISPBASE2);
		dsi1_hvs_write(SCALER_DISPBASE1, HVS4_DISPBASE1);
		dsi1_hvs_write(SCALER_DISPBASE0, HVS4_DISPBASE0);
	}

	/* Reset the channel (write 0, RESET, 0 — vc4_hvs_init_channel). */
	dsi1_hvs_write(REG_DISPCTRLX(ch), 0);
	dsi1_hvs_write(REG_DISPCTRLX(ch), SCALER_DISPCTRLX_RESET);
	bcm283x_dsi1_udelay(10);
	dsi1_hvs_write(REG_DISPCTRLX(ch), 0);

	/* Write the dlist, then point the channel at it. */
	dlist_word_idx = _write_dlist(phy_fb, w, h, dep, pitch, gen5);
	_w = w;
	_h = h;
	_dlist_idx = dlist_word_idx;
	dsi1_hvs_write(REG_DISPLISTX(ch), dlist_word_idx);

	/*
	 * Enable the channel with panel size.  Written BEFORE DISPBKGND,
	 * matching upstream order.  Size field layout differs per gen.
	 */
	if (gen5) {
		dispctrl = SCALER_DISPCTRLX_ENABLE |
				(w << SCALER5_DISPCTRLX_WIDTH_SHIFT) |
				(h << SCALER5_DISPCTRLX_HEIGHT_SHIFT);
	} else {
		dispctrl = SCALER_DISPCTRLX_ENABLE |
				(w << SCALER_DISPCTRLX_WIDTH_SHIFT) |
				(h << SCALER_DISPCTRLX_HEIGHT_SHIFT);
	}
	dsi1_hvs_write(REG_DISPCTRLX(ch), dispctrl);

	/*
	 * Background fill.  gen4 additionally wants AUTOHS (bit 31),
	 * which on gen5 is the BCK2BCK bit and must stay clear —
	 * upstream does a read-modify-write `&= ~BCK2BCK` there.
	 *
	 * Background colour is deliberately RED (0x00ff0000): the panel
	 * itself becomes a tri-state diagnostic —
	 *   red   = channel output reaches the panel but the plane is
	 *           skipped (background shows through);
	 *   black = plane composited but fetching zeros;
	 *   bars  = plane composited from our framebuffer (good).
	 * A valid plane covers the whole screen so the red never shows
	 * in the good case.
	 */
	dispbkgnd = dsi1_hvs_read(REG_DISPBKGNDX(ch));
	if (gen5) {
		dispbkgnd &= ~SCALER5_DISPBKGND_BCK2BCK;
	} else {
		dispbkgnd |= SCALER_DISPBKGND_AUTOHS;
	}
	dispbkgnd |= SCALER_DISPBKGND_FILL | 0x00ff0000U;
	dsi1_hvs_write(REG_DISPBKGNDX(ch), dispbkgnd);

	bcm283x_dsi1_udelay(100);
	return 0;
}

static uint32_t _frame_count(void) {
	uint32_t ch = _channel();
	uint32_t v;

	if (ch == 2) {
		v = dsi1_hvs_read(SCALER_DISPSTAT2);
		if (bcm283x_dsi1_is_gen5()) {
			return (v & SCALER5_DISPSTAT2_FRCNT2_MASK) >>
					SCALER5_DISPSTAT2_FRCNT2_SHIFT;
		}
		/*
		 * HVS4 has no FRCNT2 (frame counters only exist in
		 * DISPSTAT1, for channels 0/1).  Use mode + current
		 * line as the motion signature: a scanning channel
		 * changes LINE constantly, a stalled one never does.
		 */
		return v & (SCALER_DISPSTATX_MODE_MASK | 0xfffU);
	}
	v = dsi1_hvs_read(SCALER_DISPSTAT1);
	if (bcm283x_dsi1_is_gen5()) {
		if (ch == 0) {
			return (v & SCALER5_DISPSTAT1_FRCNT0_MASK) >>
					SCALER5_DISPSTAT1_FRCNT0_SHIFT;
		}
		return (v & SCALER5_DISPSTAT1_FRCNT1_MASK) >>
				SCALER5_DISPSTAT1_FRCNT1_SHIFT;
	}
	if (ch == 0) {
		return (v & SCALER_DISPSTAT1_FRCNT0_MASK) >>
				SCALER_DISPSTAT1_FRCNT0_SHIFT;
	}
	return (v & SCALER_DISPSTAT1_FRCNT1_MASK) >>
			SCALER_DISPSTAT1_FRCNT1_SHIFT;
}

static int _channel_running(void) {
	uint32_t mode = (dsi1_hvs_read(REG_DISPSTATX(_channel())) &
			SCALER_DISPSTATX_MODE_MASK) >>
			SCALER_DISPSTATX_MODE_SHIFT;
	return (mode == SCALER_DISPSTATX_MODE_RUN ||
		mode == SCALER_DISPSTATX_MODE_EOF) ? 0 : -1;
}

/*
 * Early-exit poll: bring-up only needs the FIRST positive observation,
 * and at ~60Hz that lands within one or two frames (~17-35ms), not the
 * full timeout budget.  Elapsed time comes from the 1MHz system timer;
 * the iteration cap is a backstop for coarse usleep() granularity.
 */
int bcm283x_dsi1_hvs_wait_running(uint32_t timeout_ms) {
	uint32_t start = dsi1_micros();
	uint32_t i;

	if (_mmio_base == 0) {
		return -1;
	}
	for (i = 0; i <= timeout_ms; ++i) {
		if (_channel_running() == 0) {
			return 0;
		}
		if ((uint32_t)(dsi1_micros() - start) >= timeout_ms * 1000U) {
			break;
		}
		bcm283x_dsi1_mdelay(1);
	}
	return _channel_running();
}

int bcm283x_dsi1_hvs_frames_advancing(uint32_t wait_ms) {
	uint32_t c0;
	uint32_t c1;
	uint32_t start;
	uint32_t i;

	if (_mmio_base == 0) {
		return -1;
	}
	c0 = _frame_count();
	start = dsi1_micros();
	for (i = 0; i <= wait_ms; ++i) {
		c1 = _frame_count();
		if (c1 != c0) {
			return 0;
		}
		if ((uint32_t)(dsi1_micros() - start) >= wait_ms * 1000U) {
			break;
		}
		bcm283x_dsi1_mdelay(1);
	}
	return -1;
}

/*
 * Empirical PV->FIFO crossbar probe for gen4 DSI1.  Upstream says
 * PV1's vstart lands on FIFO 2, but when an enabled, scanning PV1
 * leaves channel 2 in INIT, enable channel 0 with the same dlist
 * and watch for the vstart there.  PV0 is not enabled in this
 * configuration, so channel 0 can only leave INIT if PV1's vstart
 * wire actually feeds FIFO 0 on this silicon.  On success the
 * scan-out channel is re-targeted to 0 for the rest of the session
 * (FRCNT0 then also serves frames_advancing).  Returns 0 on switch.
 */
int bcm283x_dsi1_hvs_crossbar_probe(void) {
	uint32_t mode;
	uint32_t i;

	if (_mmio_base == 0 || _dlist_idx == 0 || _w == 0) {
		return -1;
	}
	if (dsi1_port() != 1 || bcm283x_dsi1_is_gen5()) {
		return -1;
	}

	/* vc4_hvs_init_channel sequence on channel 0, same dlist. */
	dsi1_hvs_write(REG_DISPCTRLX(0), 0);
	dsi1_hvs_write(REG_DISPCTRLX(0), SCALER_DISPCTRLX_RESET);
	bcm283x_dsi1_udelay(10);
	dsi1_hvs_write(REG_DISPCTRLX(0), 0);
	dsi1_hvs_write(REG_DISPLISTX(0), _dlist_idx);
	dsi1_hvs_write(REG_DISPCTRLX(0), SCALER_DISPCTRLX_ENABLE |
			(_w << SCALER_DISPCTRLX_WIDTH_SHIFT) |
			(_h << SCALER_DISPCTRLX_HEIGHT_SHIFT));
	dsi1_hvs_write(REG_DISPBKGNDX(0), SCALER_DISPBKGND_AUTOHS |
			SCALER_DISPBKGND_FILL | 0x00ff0000U);

	for (i = 0; i < 300; ++i) {
		mode = (dsi1_hvs_read(SCALER_DISPSTAT0) &
				SCALER_DISPSTATX_MODE_MASK) >>
				SCALER_DISPSTATX_MODE_SHIFT;
		if (mode == SCALER_DISPSTATX_MODE_RUN ||
				mode == SCALER_DISPSTATX_MODE_EOF) {
			_channel_override = 0;
			printf("dsi: vstart on FIFO0: scanout -> ch0\n");
			return 0;
		}
		bcm283x_dsi1_mdelay(1);
	}
	printf("dsi: crossbar probe: S0=%08x S2=%08x\n",
			(unsigned)dsi1_hvs_read(SCALER_DISPSTAT0),
			(unsigned)dsi1_hvs_read(SCALER_DISPSTAT2));
	return -1;
}

/*
 * Runtime plane-fetch probe.  The HVS re-parses the dlist every frame
 * and REWRITES the two context words (we pre-filled 0xC0C0C0C0):
 *   1 = context words untouched: HVS is SKIPPING our plane
 *   0 = plane processed (fetch health not discriminated further here)
 */
int dsi1_hvs_plane_touched(void) {
	if (_mmio_base == 0 || _dl_base == 0) {
		return -1;
	}
	if (dsi1_hvs_read(_dl_ctx_off) == 0xC0C0C0C0U &&
	    dsi1_hvs_read(_dl_ptrctx_off) == 0xC0C0C0C0U) {
		return 1;
	}
	return 0;
}

/*
 * Hot-swap PTR0 in the live dlist (re-read each frame).  Also refresh
 * the context words so a later probe reflects the NEW address.
 */
void dsi1_hvs_set_ptr0(uint32_t bus_addr) {
	if (_mmio_base == 0 || _dl_base == 0) {
		return;
	}
	dsi1_hvs_write(_dl_ptr0_off, bus_addr);
	dsi1_hvs_write(_dl_ctx_off, 0xC0C0C0C0U);
	dsi1_hvs_write(_dl_ptrctx_off, 0xC0C0C0C0U);
}

/*
 * One-line HVS truth dump for the boot log, sampled while the
 * pipeline is live:
 *   list  = DISPLISTX readback (someone else rewrote it => firmware
 *           still owns the channel)
 *   ctl0/ptr0 = dlist words read back from HVS SRAM (corruption or
 *           wrong SRAM base shows up here)
 *   stat  = DISPSTATX (mode/line for the channel)
 *   plane = dsi1_hvs_plane_touched(): 0 the HVS rewrote the context
 *           words (plane composited), 1 untouched (plane SKIPPED)
 */
void dsi1_hvs_dump_live(void) {
	uint32_t ch = _channel();

	if (_mmio_base == 0 || _dl_base == 0) {
		return;
	}
	printf("hvs%u: list=%u ctl0=%08x ptr0=%08x stat=%08x plane=%d\n",
			(unsigned)ch,
			(unsigned)dsi1_hvs_read(REG_DISPLISTX(ch)),
			(unsigned)dsi1_hvs_read(_dl_base),
			(unsigned)dsi1_hvs_read(_dl_ptr0_off),
			(unsigned)dsi1_hvs_read(REG_DISPSTATX(ch)),
			dsi1_hvs_plane_touched());
}
