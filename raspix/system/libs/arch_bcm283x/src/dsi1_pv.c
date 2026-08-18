#include "dsi1_internal.h"

#include <stdint.h>

/*
 * PixelValve1 (MMIO+0x207000) — the DSI1 encoder's pixel source.
 * Mirrors vc4_crtc_config_pv(): programs every PV register EXCEPT the
 * PV_CONTROL_EN and PV_VCONTROL_VIDEN kick bits, which upstream splits
 * out to vc4_crtc_atomic_enable so they can be ordered around DSI
 * DISP0_ENABLE.
 */

#define PV_CONTROL             0x00U
#define  PV_CONTROL_FORMAT_SHIFT    21
#define  PV_CONTROL_FORMAT_24       0U
#define  PV_CONTROL_FORMAT_DSIV_24  4U
#define  PV_CONTROL_FIFO_LEVEL_SHIFT 15
#define  PV_CONTROL_CLR_AT_START    (1U << 14)
#define  PV_CONTROL_TRIGGER_UNDERFLOW (1U << 13)
#define  PV_CONTROL_WAIT_HSTART       (1U << 12)
#define  PV_CONTROL_CLK_SELECT_SHIFT  2
#define  PV_CONTROL_CLK_SELECT_DSI    0U
#define  PV_CONTROL_FIFO_CLR          (1U << 1)
#define  PV_CONTROL_EN                (1U << 0)
/* HVS5 (BCM2711) high FIFO level, 2 bits at 26:25. */
#define  PV5_CONTROL_FIFO_LEVEL_HIGH_SHIFT 25

#define PV_V_CONTROL           0x04U
#define  PV_VCONTROL_DSI         (1U << 3)
#define  PV_VCONTROL_CONTINUOUS  (1U << 1)
#define  PV_VCONTROL_VIDEN       (1U << 0)

#define PV_VSYNCD_EVEN         0x08U

#define PV_HORZA               0x0cU
#define  PV_HORZA_HBP_SHIFT      16
#define  PV_HORZA_HSYNC_SHIFT    0

#define PV_HORZB               0x10U
#define  PV_HORZB_HFP_SHIFT      16
#define  PV_HORZB_HACTIVE_SHIFT  0

#define PV_VERTA               0x14U
#define  PV_VERTA_VBP_SHIFT      16
#define  PV_VERTA_VSYNC_SHIFT    0

#define PV_VERTB               0x18U
#define  PV_VERTB_VFP_SHIFT      16
#define  PV_VERTB_VACTIVE_SHIFT  0

#define PV_HACT_ACT            0x30U

/* HVS5-only: no RGB pixel swap. */
#define PV_MUX_CFG             0x34U
#define  PV_MUX_CFG_RGB_PIXEL_MUX_MODE_SHIFT  2
#define  PV_MUX_CFG_RGB_PIXEL_MUX_MODE_NO_SWAP 8U

/*
 * PV1 fifo_full_level for DSIV_24 (vc4_get_fifo_full_level): PV1's
 * fifo_depth is 64 bytes on both gens and HVS_FIFO_LATENCY_PIX = 6.
 *   gen4: 64 - 3*6 - 1 = 45   (the gen4 formula subtracts one more)
 *   gen5: 64 - 3*6     = 46
 * The low 6 bits go to PV_CONTROL_FIFO_LEVEL, the high 2 bits to
 * PV5_CONTROL_FIFO_LEVEL_HIGH (only wired on gen5; 45/46 both fit
 * the low 6 bits anyway).
 */
#define PV1_FIFO_DEPTH         64U
#define HVS_FIFO_LATENCY_PIX   6U
#define PV1_FIFO_FULL_LEVEL_GEN4  (PV1_FIFO_DEPTH - 3U * HVS_FIFO_LATENCY_PIX - 1U)
#define PV1_FIFO_FULL_LEVEL_GEN5  (PV1_FIFO_DEPTH - 3U * HVS_FIFO_LATENCY_PIX)

int bcm283x_dsi1_pv_configure(const bcm283x_dsi1_adjusted_mode_t* mode) {
	int gen5 = bcm283x_dsi1_is_gen5();
	uint32_t fifo_level;
	uint32_t control;
	/*
	 * vc4_crtc_config_pv(): `format = is_dsi1 ? PV_CONTROL_FORMAT_DSIV_24
	 * : PV_CONTROL_FORMAT_24` — only the DSI1 encoder takes the DSIV
	 * pixel packing; DSI0's older block wants plain FORMAT_24.  Both
	 * land in the same fifo-full-level branch, so only FORMAT differs.
	 */
	uint32_t format = dsi1_port() ?
			PV_CONTROL_FORMAT_DSIV_24 : PV_CONTROL_FORMAT_24;

	if (mode == 0) {
		return -1;
	}
	fifo_level = gen5 ? PV1_FIFO_FULL_LEVEL_GEN5 : PV1_FIFO_FULL_LEVEL_GEN4;

	/* vc4_crtc_pixelvalve_reset(): disable, then clear FIFO. */
	dsi1_pv_write(PV_CONTROL, dsi1_pv_read(PV_CONTROL) & ~PV_CONTROL_EN);
	dsi1_pv_write(PV_CONTROL, dsi1_pv_read(PV_CONTROL) | PV_CONTROL_FIFO_CLR);

	/* Horizontal: HBP | HSYNC in HORZA, HFP | HACTIVE in HORZB. */
	dsi1_pv_write(PV_HORZA,
			(mode->hbp << PV_HORZA_HBP_SHIFT) |
			(mode->hsw << PV_HORZA_HSYNC_SHIFT));
	dsi1_pv_write(PV_HORZB,
			(mode->hfp   << PV_HORZB_HFP_SHIFT) |
			(mode->width << PV_HORZB_HACTIVE_SHIFT));

	/* Vertical. */
	dsi1_pv_write(PV_VERTA,
			(mode->vbp << PV_VERTA_VBP_SHIFT) |
			(mode->vsw << PV_VERTA_VSYNC_SHIFT));
	dsi1_pv_write(PV_VERTB,
			(mode->vfp    << PV_VERTB_VFP_SHIFT) |
			(mode->height << PV_VERTB_VACTIVE_SHIFT));

	/* DSI needs an HACT_ACT hint (pixel_rep=1 -> hdisplay). */
	dsi1_pv_write(PV_HACT_ACT, mode->width);

	/* HVS5-only register: no RGB pixel swap. */
	if (gen5) {
		dsi1_pv_write(PV_MUX_CFG,
				PV_MUX_CFG_RGB_PIXEL_MUX_MODE_NO_SWAP <<
				PV_MUX_CFG_RGB_PIXEL_MUX_MODE_SHIFT);
	}

	/*
	 * V_CONTROL: continuous non-interlaced, DSI encoder.  Upstream
	 * writes VIDEN=0 here and only ORs VIDEN in later.
	 */
	dsi1_pv_write(PV_V_CONTROL,
			PV_VCONTROL_CONTINUOUS | PV_VCONTROL_DSI);
	dsi1_pv_write(PV_VSYNCD_EVEN, 0);

	/*
	 * Control: per-port FORMAT, CLK_SELECT=DSI, wait-hstart, correct
	 * FIFO full level.  EN stays clear here; upstream ORs it in from
	 * vc4_crtc_atomic_enable.
	 */
	control = PV_CONTROL_FIFO_CLR |
		  (format << PV_CONTROL_FORMAT_SHIFT) |
		  (PV_CONTROL_CLK_SELECT_DSI << PV_CONTROL_CLK_SELECT_SHIFT) |
		  PV_CONTROL_CLR_AT_START |
		  PV_CONTROL_TRIGGER_UNDERFLOW |
		  PV_CONTROL_WAIT_HSTART |
		  ((fifo_level & 0x3fU) << PV_CONTROL_FIFO_LEVEL_SHIFT) |
		  (((fifo_level >> 6) & 0x3U) << PV5_CONTROL_FIFO_LEVEL_HIGH_SHIFT);
	dsi1_pv_write(PV_CONTROL, control);
	return 0;
}

/* CRTC_WRITE(PV_CONTROL, CRTC_READ(PV_CONTROL) | PV_CONTROL_EN) */
int bcm283x_dsi1_pv_enable(void) {
	dsi1_pv_write(PV_CONTROL, dsi1_pv_read(PV_CONTROL) | PV_CONTROL_EN);
	return 0;
}

/* CRTC_WRITE(PV_V_CONTROL, CRTC_READ(PV_V_CONTROL) | PV_VCONTROL_VIDEN) */
int bcm283x_dsi1_pv_video_enable(void) {
	dsi1_pv_write(PV_V_CONTROL, dsi1_pv_read(PV_V_CONTROL) | PV_VCONTROL_VIDEN);
	return 0;
}

/*
 * Diagnostic fallback: with WAIT_HSTART set the PV gates every line
 * (and thus the whole frame, vstart included) on the hstart handshake
 * from the DSI host.  If that handshake never arrives the channel
 * stays INIT forever; clearing the bit lets the PV free-run its own
 * timings so the two cases are distinguishable at runtime.
 */
int bcm283x_dsi1_pv_clear_wait_hstart(void) {
	dsi1_pv_write(PV_CONTROL,
			dsi1_pv_read(PV_CONTROL) & ~PV_CONTROL_WAIT_HSTART);
	return 0;
}
