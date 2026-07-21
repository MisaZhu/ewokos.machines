#include "uc_pv.h"
#include "uc_cwu50.h"
#include "uc_time.h"

#include <stdint.h>

#include <ewoksys/mmio.h>

#include "uc_log.h"
#define slog uc_log

/* ---------- PV register offsets (from Linux drivers/gpu/drm/vc4). --- */

#define PV_CONTROL             0x00U
#define  PV_CONTROL_FORMAT_SHIFT    21
#define  PV_CONTROL_FORMAT_DSIV_24  4U
#define  PV_CONTROL_FIFO_LEVEL_SHIFT 15
#define  PV_CONTROL_CLR_AT_START    (1U << 14)
#define  PV_CONTROL_TRIGGER_UNDERFLOW (1U << 13)
#define  PV_CONTROL_WAIT_HSTART       (1U << 12)
#define  PV_CONTROL_CLK_SELECT_SHIFT  2
#define  PV_CONTROL_CLK_SELECT_DSI    0U
#define  PV_CONTROL_FIFO_CLR          (1U << 1)
#define  PV_CONTROL_EN                (1U << 0)
/* HVS5 (BCM2711) high FIFO level, 2 bits at 26:25 */
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

#define PV_MUX_CFG             0x34U
#define  PV_MUX_CFG_RGB_PIXEL_MUX_MODE_SHIFT  2
#define  PV_MUX_CFG_RGB_PIXEL_MUX_MODE_NO_SWAP 8U

static volatile uint32_t* _pv1 = 0;

static void _pv_init(void) {
	if (_pv1 == 0 && _mmio_base != 0) {
		_pv1 = (volatile uint32_t*)(uintptr_t)(_mmio_base + UC_PV1_OFFSET);
	}
}

static uint32_t _pv_read(uint32_t off) {
	if (_pv1 == 0) return 0;
	return _pv1[off / 4];
}

static void _pv_write(uint32_t off, uint32_t v) {
	if (_pv1 == 0) return;
	_pv1[off / 4] = v;
}

/*
 * PV1 fifo_full_level for DSIV_24 on hvs5 (per vc4_get_fifo_full_level):
 * bcm2711_pv1_data.hvs_output == 3 (not 5), so we fall through the
 * hvs5 branch which returns `fifo_depth - 3 * HVS_FIFO_LATENCY_PIX`
 * = 64 - 3*6 = 46. The low 6 bits go to PV_CONTROL_FIFO_LEVEL, the
 * high 2 bits to PV5_CONTROL_FIFO_LEVEL_HIGH.
 */
#define UC_PV1_FIFO_LATENCY_PIX  6U
#define UC_PV1_FIFO_DEPTH        64U
#define UC_PV1_FIFO_FULL_LEVEL   (UC_PV1_FIFO_DEPTH - 3U * UC_PV1_FIFO_LATENCY_PIX)

/*
 * Program PV1 registers for cwu50 timing.  Mirrors vc4_crtc_config_pv():
 * writes every PV register EXCEPT the PV_CONTROL_EN and
 * PV_VCONTROL_VIDEN kick bits, which upstream splits out to
 * vc4_crtc_atomic_enable so they can be ordered around DSI
 * DISP0_ENABLE.
 *
 * cwu50: 720x1280, hfp=43/hsw=20/hbp=20, vfp=8/vsw=2/vbp=16.
 */
int uc_pv_configure(void) {
	uint32_t control;

	_pv_init();
	if (_pv1 == 0) return -1;

	/* vc4_crtc_pixelvalve_reset(): disable, then clear FIFO. */
	_pv_write(PV_CONTROL, _pv_read(PV_CONTROL) & ~PV_CONTROL_EN);
	_pv_write(PV_CONTROL, _pv_read(PV_CONTROL) | PV_CONTROL_FIFO_CLR);

	/* Horizontal: HBP | HSYNC in HORZA, HFP | HACTIVE in HORZB. */
	_pv_write(PV_HORZA,
			(UC_CWU50_H_BP << PV_HORZA_HBP_SHIFT) |
			(UC_CWU50_H_SW << PV_HORZA_HSYNC_SHIFT));
	_pv_write(PV_HORZB,
			(UC_CWU50_H_FP     << PV_HORZB_HFP_SHIFT) |
			(UC_CWU50_H_ACTIVE << PV_HORZB_HACTIVE_SHIFT));

	/* Vertical. */
	_pv_write(PV_VERTA,
			(UC_CWU50_V_BP << PV_VERTA_VBP_SHIFT) |
			(UC_CWU50_V_SW << PV_VERTA_VSYNC_SHIFT));
	_pv_write(PV_VERTB,
			(UC_CWU50_V_FP     << PV_VERTB_VFP_SHIFT) |
			(UC_CWU50_V_ACTIVE << PV_VERTB_VACTIVE_SHIFT));

	/* DSI needs an HACT_ACT hint (pixel_rep=1 -> hdisplay). */
	_pv_write(PV_HACT_ACT, UC_CWU50_H_ACTIVE);

	/* HVS5: no RGB pixel swap. */
	_pv_write(PV_MUX_CFG,
			PV_MUX_CFG_RGB_PIXEL_MUX_MODE_NO_SWAP <<
			PV_MUX_CFG_RGB_PIXEL_MUX_MODE_SHIFT);

	/*
	 * V_CONTROL: continuous non-interlaced, DSI encoder.  Upstream
	 * writes VIDEN=0 here and only ORs VIDEN in later.
	 */
	_pv_write(PV_V_CONTROL,
			PV_VCONTROL_CONTINUOUS | PV_VCONTROL_DSI);
	_pv_write(PV_VSYNCD_EVEN, 0);

	/*
	 * Control: DSIV_24, CLK_SELECT=DSI, wait-hstart, correct FIFO
	 * full level (46 for PV1/hvs5).  EN stays clear here; upstream
	 * ORs it in from vc4_crtc_atomic_enable.
	 */
	control = PV_CONTROL_FIFO_CLR |
	          (PV_CONTROL_FORMAT_DSIV_24 << PV_CONTROL_FORMAT_SHIFT) |
	          (PV_CONTROL_CLK_SELECT_DSI << PV_CONTROL_CLK_SELECT_SHIFT) |
	          PV_CONTROL_CLR_AT_START |
	          PV_CONTROL_TRIGGER_UNDERFLOW |
	          PV_CONTROL_WAIT_HSTART |
	          ((UC_PV1_FIFO_FULL_LEVEL & 0x3fU) << PV_CONTROL_FIFO_LEVEL_SHIFT) |
	          (((UC_PV1_FIFO_FULL_LEVEL >> 6) & 0x3U) << PV5_CONTROL_FIFO_LEVEL_HIGH_SHIFT);
	_pv_write(PV_CONTROL, control);
	return 0;
}

/* CRTC_WRITE(PV_CONTROL, CRTC_READ(PV_CONTROL) | PV_CONTROL_EN) */
int uc_pv_enable(void) {
	_pv_init();
	if (_pv1 == 0) return -1;
	_pv_write(PV_CONTROL, _pv_read(PV_CONTROL) | PV_CONTROL_EN);
	return 0;
}

/* CRTC_WRITE(PV_V_CONTROL, CRTC_READ(PV_V_CONTROL) | PV_VCONTROL_VIDEN) */
int uc_pv_video_enable(void) {
	_pv_init();
	if (_pv1 == 0) return -1;
	_pv_write(PV_V_CONTROL, _pv_read(PV_V_CONTROL) | PV_VCONTROL_VIDEN);
	return 0;
}
