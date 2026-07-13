#include "uc_hvs.h"
#include "uc_time.h"

#include <stdint.h>

#include <ewoksys/mmio.h>

/* Silent operation. */
#define slog(...) ((void)0)

/* ---------- HVS register offsets (from Linux drivers/gpu/drm/vc4). --- */

#define SCALER_DISPCTRL           0x00000000U
#define  SCALER_DISPCTRL_ENABLE     (1U << 31)

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
	uint32_t base = SCALER5_DLIST_START;
	uint32_t hvs_fmt;
	uint32_t hvs_order = HVS_PIXEL_ORDER_ARGB;
	uint32_t pitch;
	uint32_t ctl0;
	uint32_t size_words = 8;
	uint32_t idx = 0;

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
	/* PTR0: physical / bus FB address. */
	_hvs_write(base + (idx++) * 4U, phy_fb);
	/* Context slot for PTR. */
	_hvs_write(base + (idx++) * 4U, 0xC0C0C0C0U);
	/* PITCH0. */
	_hvs_write(base + (idx++) * 4U, pitch);
	/* End of dlist. */
	_hvs_write(base + (idx++) * 4U, SCALER_CTL0_END);

	return 0;  /* dlist starts at word index 0 */
}

int uc_hvs_bringup(uint32_t phy_fb, uint32_t w, uint32_t h, uint32_t dep) {
	uint32_t dlist_word_idx;
	uint32_t dispctrl;

	_hvs_init();
	if (_hvs == 0) return -1;

	slog("[uc_hvs] bringup fb=0x%08x %ux%u@%ubpp\n", phy_fb, w, h, dep);

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
	 */
	_hvs_write(SCALER_DISPBKGND1,
			SCALER_DISPBKGND_AUTOHS |
			SCALER_DISPBKGND_FILL |
			0x00000000U);

	uc_udelay(100);
	slog("[uc_hvs] STAT1=0x%08x CTRL1=0x%08x LIST1=0x%08x\n",
			_hvs_read(SCALER_DISPSTAT1),
			_hvs_read(SCALER_DISPCTRL1),
			_hvs_read(SCALER_DISPLIST1));
	return 0;
}
