#include "uc_dsi.h"
#include "uc_time.h"

#include <stdint.h>

#include <ewoksys/mmio.h>

static volatile uint32_t* _dsi1 = 0;

void uc_dsi_init(void) {
	if (_dsi1 == 0 && _mmio_base != 0) {
		_dsi1 = (volatile uint32_t*)(uintptr_t)(_mmio_base + UC_DSI1_OFFSET);
	}
}

uint32_t uc_dsi1_read(uint32_t off) {
	uc_dsi_init();
	if (_dsi1 == 0) {
		return 0;
	}
	return _dsi1[off / 4];
}

void uc_dsi1_write(uint32_t off, uint32_t val) {
	uc_dsi_init();
	if (_dsi1 == 0) {
		return;
	}
	_dsi1[off / 4] = val;
}

void uc_dsi_dump(void) {
}

/* --------- Additional DSI1 register bits used only inside this file. --- */

/* CTRL bits. */
#define DSI1_CTRL_HSDT_EOT_DISABLE       (1U << 11)
#define DSI1_CTRL_RX_LPDT_EOT_DISABLE    (1U << 13)
#define DSI1_CTRL_SOFT_RESET_CFG         (1U << 10)
#define DSI1_CTRL_CAL_BYTE               (1U << 9)
#define DSI1_CTRL_CLR_LDF                (1U << 7)
#define DSI1_CTRL_CLR_RXF                (1U << 6)
#define DSI1_CTRL_CLR_PDF                (1U << 5)
#define DSI1_CTRL_CLR_CDF                (1U << 4)
#define DSI1_CTRL_RESET_FIFOS   (DSI1_CTRL_CLR_LDF | DSI1_CTRL_CLR_RXF | \
				 DSI1_CTRL_CLR_PDF | DSI1_CTRL_CLR_CDF)

/* PHYC (DSI1 variant) — the DLANE*_ENABLE bits are already in uc_dsi.h. */
#define DSI1_PHYC_ESC_CLK_LPDT_SHIFT     20
#define DSI1_PHYC_ESC_CLK_LPDT_MASK      (0x3fU << 20)

/* PHY_AFEC0 — DSI1 variant (see vc4_dsi.c). */
#define DSI1_PHY_AFEC0_RESET             (1U << 13)
#define DSI1_PHY_AFEC0_PD_DLANE1         (1U << 10)
#define DSI1_PHY_AFEC0_PD_DLANE2         (1U << 9)
#define DSI1_PHY_AFEC0_PD_DLANE3         (1U << 8)
#define DSI_PHY_AFEC0_PTATADJ_SHIFT      4
#define DSI_PHY_AFEC0_CTATADJ_SHIFT      0
#define DSI1_PHY_AFEC0_IDR_CLANE_SHIFT   17
#define DSI1_PHY_AFEC0_IDR_DLANE0_SHIFT  20
#define DSI1_PHY_AFEC0_IDR_DLANE1_SHIFT  23
#define DSI1_PHY_AFEC0_IDR_DLANE2_SHIFT  26
#define DSI1_PHY_AFEC0_IDR_DLANE3_SHIFT  29

/* HS_CLT0 field shifts (see vc4_dsi.c DSI_HS_CLT0_*). */
#define HS_CLT0_CZERO_SHIFT              18
#define HS_CLT0_CPRE_SHIFT               9
#define HS_CLT0_CPREP_SHIFT              0
/* HS_CLT1. */
#define HS_CLT1_CTRAIL_SHIFT             9
#define HS_CLT1_CPOST_SHIFT              0
/* HS_CLT2. */
#define HS_CLT2_WUP_SHIFT                0
/* HS_DLT3. */
#define HS_DLT3_EXIT_SHIFT               18
#define HS_DLT3_ZERO_SHIFT               9
#define HS_DLT3_PRE_SHIFT                0
/* HS_DLT4. */
#define HS_DLT4_ANLAT_SHIFT              18
#define HS_DLT4_TRAIL_SHIFT              9
#define HS_DLT4_LPX_SHIFT                0
/* HS_DLT5. */
#define HS_DLT5_INIT_SHIFT               0
/* HS_DLT6. */
#define HS_DLT6_TA_GET_SHIFT             24
#define HS_DLT6_TA_SURE_SHIFT            16
#define HS_DLT6_TA_GO_SHIFT              8
#define HS_DLT6_LP_LPX_SHIFT             0
/* HS_DLT7. */
#define HS_DLT7_LP_WUP_SHIFT             0

/* DISP1_CTRL. */
#define DISP1_PFORMAT_32BIT_LE_SHIFT     1
#define DISP1_PFORMAT_32BIT_LE_VAL       2
#define DISP1_ENABLE                     (1U << 0)

/* DISP0_CTRL. */
#define DISP0_COMMAND_MODE               (1U << 1)
#define DISP0_ENABLE                     (1U << 0)

/* TXPKT1C fields. */
#define TXPKT1C_CMD_EN                   (1U << 0)
#define TXPKT1C_CMD_TYPE_LONG            (1U << 2)
#define TXPKT1C_CMD_MODE_LP              (1U << 3)
#define TXPKT1C_CMD_CTRL_TX              (0U << 4)
#define TXPKT1C_CMD_CTRL_RX              (1U << 4)
#define TXPKT1C_CMD_REPEAT_SHIFT         10
#define TXPKT1C_DISPLAY_NO_SHORT         (0U << 8)

/* RXPKT1H fields (vc4_dsi.c DSI_RXPKT1H_*). */
#define RXPKT1H_PKT_TYPE_LONG            (1U << 24)
#define RXPKT1H_BC_PARAM_SHIFT           8
#define RXPKT1H_BC_PARAM_MASK            0xffffU
#define RXPKT1H_SHORT_0_SHIFT            8
#define RXPKT1H_SHORT_1_SHIFT            16

/* TXPKT1H fields. */
#define TXPKT1H_BC_DT_SHIFT              0
#define TXPKT1H_BC_PARAM_SHIFT           8
#define TXPKT1H_BC_CMDFIFO_SHIFT         24

/*
 * Little helper: unit-interval-in-ns for our target HS bit clock.
 * Note: PHY clock is DDR, so 1 UI = 2 clock periods, hence 500e6/hz.
 */
static uint32_t _ui_ns(void) {
	return (500000000U + UC_DSI_HS_CLOCK_HZ - 1U) / UC_DSI_HS_CLOCK_HZ;
}

/* From vc4_dsi.c::dsi_hs_timing().  Round up to a multiple of 8 (byte clock). */
static uint32_t _hst(uint32_t ui_ns, uint32_t ns, uint32_t ui) {
	uint32_t v = ui + ((ns + ui_ns - 1U) / ui_ns);
	return (v + 7U) & ~7U;
}

/* ESC clock always assumed 100 MHz => 10 ns per tick. */
static uint32_t _est(uint32_t ns) {
	return (ns + 9U) / 10U;
}

static uint32_t _hst_max(uint32_t a, uint32_t b) {
	return a > b ? a : b;
}

void uc_dsi_ulps(int enter) {
	uint32_t phyc_ulps;
	uint32_t stat_ulps;
	uint32_t stat_stop;
	uint32_t phyc;
	int spin;
	int ulps_now;

	uc_dsi_init();
	if (_dsi1 == 0) {
		return;
	}

	/*
	 * vc4_dsi_ulps(): "if (ulps == ulps_currently_enabled) return;"
	 * where the current state is the AFEC0 LATCH_ULPS latch.  At cold
	 * boot the latch is clear, so the ulps(false) call at the end of
	 * bringup is a NO-OP upstream — it must be one here too instead
	 * of rewriting PHYC and busy-waiting on lane STOP.
	 */
	ulps_now = (uc_dsi1_read(UC_DSI1_PHY_AFEC0) &
			UC_DSI1_PHY_AFEC0_LATCH_ULPS) != 0;
	if ((enter != 0) == ulps_now) {
		return;
	}

	/* Continuous HS clock → don't put the clock lane into ULPS. */
	phyc_ulps = (1U << 1) |                    /* DLANE0 ULPS */
		    (UC_DSI_LANES > 1 ? (1U << 5)  : 0) |
		    (UC_DSI_LANES > 2 ? (1U << 9)  : 0) |
		    (UC_DSI_LANES > 3 ? (1U << 13) : 0);
	stat_ulps = UC_DSI1_STAT_PHY_D0_STOP;   /* placeholder: ULPS bits differ */
	(void)stat_ulps;
	stat_stop = UC_DSI1_STAT_PHY_D0_STOP |
		    (UC_DSI_LANES > 1 ? UC_DSI1_STAT_PHY_D1_STOP : 0) |
		    (UC_DSI_LANES > 2 ? UC_DSI1_STAT_PHY_D2_STOP : 0) |
		    (UC_DSI_LANES > 3 ? UC_DSI1_STAT_PHY_D3_STOP : 0);

	phyc = uc_dsi1_read(UC_DSI1_PHYC);
	if (enter) {
		uc_dsi1_write(UC_DSI1_PHYC, phyc | phyc_ulps);
	} else {
		uc_dsi1_write(UC_DSI1_PHYC, phyc & ~phyc_ulps);
	}

	/* Wait for STAT to reflect all lanes back in STOP.  ~200 ms budget. */
	for (spin = 0; spin < 200000; spin++) {
		if ((uc_dsi1_read(UC_DSI1_STAT) & stat_stop) == stat_stop) {
			return;
		}
		uc_udelay(1);
	}
}

/*
 * Same liveness check vc4_dsi's probe does: the ID register must read
 * 0x00647369 ("dsi").  If the firmware power domain for DSI1 is off,
 * the register bus reads back garbage (usually 0) and nothing else in
 * this driver can possibly work.
 */
int uc_dsi_alive(void) {
	uc_dsi_init();
	if (_dsi1 == 0) {
		return -1;
	}
	return (uc_dsi1_read(UC_DSI1_ID) == 0x00647369U) ? 0 : -1;
}

/*
 * After bringup all enabled data lanes must sit in LP-11 STOP; that is
 * the analog PHY actually driving the lines.  Mirrors the STAT check
 * uc_dsi_ulps() polls for.
 */
int uc_dsi_lanes_stopped(void) {
	uint32_t stat_stop;

	uc_dsi_init();
	if (_dsi1 == 0) {
		return -1;
	}
	stat_stop = UC_DSI1_STAT_PHY_D0_STOP |
		    (UC_DSI_LANES > 1 ? UC_DSI1_STAT_PHY_D1_STOP : 0) |
		    (UC_DSI_LANES > 2 ? UC_DSI1_STAT_PHY_D2_STOP : 0) |
		    (UC_DSI_LANES > 3 ? UC_DSI1_STAT_PHY_D3_STOP : 0);
	return ((uc_dsi1_read(UC_DSI1_STAT) & stat_stop) == stat_stop) ? 0 : -1;
}

int uc_dsi_bringup(void) {
	uint32_t ui_ns;
	uint32_t lpx;
	uint32_t afec0;

	uc_dsi_init();
	if (_dsi1 == 0) {
		return -1;
	}

	ui_ns = _ui_ns();
	lpx = _est(60);   /* Minimum LP state = 60ns => 6 escape ticks. */

	/* Reset the controller + all FIFOs. */
	uc_dsi1_write(UC_DSI1_CTRL, DSI1_CTRL_SOFT_RESET_CFG | DSI1_CTRL_RESET_FIFOS);
	uc_dsi1_write(UC_DSI1_CTRL, DSI1_CTRL_HSDT_EOT_DISABLE | DSI1_CTRL_RX_LPDT_EOT_DISABLE);

	/* Clear all STAT bits (W1C). */
	uc_dsi1_write(UC_DSI1_STAT, uc_dsi1_read(UC_DSI1_STAT));

	/*
	 * Mirror vc4_dsi bind: keep the error/timeout interrupts enabled
	 * from the start and flush any latched interrupt state.  Some of
	 * the INT_STAT reporting is gated by INT_EN, so leaving INT_EN
	 * at 0 hides transfer completion from the DCS polling loop.
	 */
	uc_dsi1_write(UC_DSI1_INT_EN, UC_DSI1_INT_ALWAYS_ENABLED);
	uc_dsi1_write(UC_DSI1_INT_STAT, uc_dsi1_read(UC_DSI1_INT_STAT));

	/*
	 * Bring the analog PHY out of powerdown, but keep AFEC0 RESET set
	 * until after the clocks have been ticking for a bit.  IDR values
	 * of 6 for all lanes matches vc4's DSI1 path.
	 */
	afec0 = (7U << DSI_PHY_AFEC0_PTATADJ_SHIFT) |
		(7U << DSI_PHY_AFEC0_CTATADJ_SHIFT) |
		(6U << DSI1_PHY_AFEC0_IDR_CLANE_SHIFT) |
		(6U << DSI1_PHY_AFEC0_IDR_DLANE0_SHIFT) |
		(6U << DSI1_PHY_AFEC0_IDR_DLANE1_SHIFT) |
		(6U << DSI1_PHY_AFEC0_IDR_DLANE2_SHIFT) |
		(6U << DSI1_PHY_AFEC0_IDR_DLANE3_SHIFT) |
		DSI1_PHY_AFEC0_RESET;
	if (UC_DSI_LANES < 4) afec0 |= DSI1_PHY_AFEC0_PD_DLANE3;
	if (UC_DSI_LANES < 3) afec0 |= DSI1_PHY_AFEC0_PD_DLANE2;
	if (UC_DSI_LANES < 2) afec0 |= DSI1_PHY_AFEC0_PD_DLANE1;
	uc_dsi1_write(UC_DSI1_PHY_AFEC0, afec0);
	uc_dsi1_write(UC_DSI1_PHY_AFEC1, 0);
	uc_mdelay(1);

	/* HS timing regs — verbatim from vc4_dsi_encoder_enable(). */
	uc_dsi1_write(UC_DSI1_HS_CLT0,
			(_hst(ui_ns, 262, 0) << HS_CLT0_CZERO_SHIFT) |
			(_hst(ui_ns, 0,   8) << HS_CLT0_CPRE_SHIFT)  |
			(_hst(ui_ns, 38,  0) << HS_CLT0_CPREP_SHIFT));
	uc_dsi1_write(UC_DSI1_HS_CLT1,
			(_hst(ui_ns, 60, 0) << HS_CLT1_CTRAIL_SHIFT) |
			(_hst(ui_ns, 60, 52) << HS_CLT1_CPOST_SHIFT));
	uc_dsi1_write(UC_DSI1_HS_CLT2,
			(_hst(ui_ns, 1000000, 0) << HS_CLT2_WUP_SHIFT));
	uc_dsi1_write(UC_DSI1_HS_DLT3,
			(_hst(ui_ns, 100, 0) << HS_DLT3_EXIT_SHIFT) |
			(_hst(ui_ns, 105, 6) << HS_DLT3_ZERO_SHIFT) |
			(_hst(ui_ns, 40,  4) << HS_DLT3_PRE_SHIFT));
	uc_dsi1_write(UC_DSI1_HS_DLT4,
			(_hst(ui_ns, lpx * 10, 0) << HS_DLT4_LPX_SHIFT) |
			(_hst_max(_hst(ui_ns, 0, 8),
				  _hst(ui_ns, 60, 4)) << HS_DLT4_TRAIL_SHIFT) |
			(0U << HS_DLT4_ANLAT_SHIFT));
	uc_dsi1_write(UC_DSI1_HS_DLT5,
			(_hst(ui_ns, 5 * 1000 * 1000, 0) << HS_DLT5_INIT_SHIFT));
	uc_dsi1_write(UC_DSI1_HS_DLT6,
			((lpx * 5) << HS_DLT6_TA_GET_SHIFT) |
			(lpx       << HS_DLT6_TA_SURE_SHIFT) |
			((lpx * 4) << HS_DLT6_TA_GO_SHIFT) |
			(lpx       << HS_DLT6_LP_LPX_SHIFT));
	uc_dsi1_write(UC_DSI1_HS_DLT7,
			(_est(1000000) << HS_DLT7_LP_WUP_SHIFT));

	/* PHYC: enable clock lane + all data lanes + continuous HS clock. */
	uc_dsi1_write(UC_DSI1_PHYC,
			UC_DSI1_PHYC_DLANE0_ENABLE |
			(UC_DSI_LANES >= 2 ? UC_DSI1_PHYC_DLANE1_ENABLE : 0) |
			(UC_DSI_LANES >= 3 ? UC_DSI1_PHYC_DLANE2_ENABLE : 0) |
			(UC_DSI_LANES >= 4 ? UC_DSI1_PHYC_DLANE3_ENABLE : 0) |
			UC_DSI1_PHYC_CLANE_ENABLE |
			UC_DSI1_PHYC_HS_CLK_CONTINUOUS |
			(((lpx - 1U) << DSI1_PHYC_ESC_CLK_LPDT_SHIFT) &
			 DSI1_PHYC_ESC_CLK_LPDT_MASK));

	/* Byte calibration. */
	uc_dsi1_write(UC_DSI1_CTRL,
			uc_dsi1_read(UC_DSI1_CTRL) | DSI1_CTRL_CAL_BYTE);

	/* Timeouts (vc4 uses disable / large fixed values). */
	uc_dsi1_write(UC_DSI1_HSTX_TO_CNT, 0);
	uc_dsi1_write(UC_DSI1_LPRX_TO_CNT, 0xffffffU);
	uc_dsi1_write(UC_DSI1_TA_TO_CNT,   100000);
	uc_dsi1_write(UC_DSI1_PR_TO_CNT,   100000);

	/* DISP1 for long command payloads through the pixel FIFO. */
	uc_dsi1_write(UC_DSI1_DISP1_CTRL,
			(DISP1_PFORMAT_32BIT_LE_VAL << DISP1_PFORMAT_32BIT_LE_SHIFT) |
			DISP1_ENABLE);

	/* Ungate the block. */
	uc_dsi1_write(UC_DSI1_CTRL,
			uc_dsi1_read(UC_DSI1_CTRL) | UC_DSI1_CTRL_EN);

	/* Release AFEC RESET. */
	uc_dsi1_write(UC_DSI1_PHY_AFEC0,
			uc_dsi1_read(UC_DSI1_PHY_AFEC0) & ~DSI1_PHY_AFEC0_RESET);

	/* Leave ULPS so the panel sees line activity.  (No-op at cold
	 * boot, exactly like upstream: LATCH_ULPS is already clear.) */
	uc_dsi_ulps(0);

	/*
	 * Upstream vc4_dsi_encoder_enable() leaves DISP0_CTRL at 0 all
	 * the way through the panel's DCS init (bridge pre_enable); the
	 * complete video-mode value INCLUDING the ENABLE bit is written
	 * in one shot afterwards.  uc_dsi_video_mode() does that write;
	 * nothing must touch DISP0_CTRL here.
	 */

	return 0;
}

static int _mipi_is_long(uint8_t dt) {
	/*
	 * MIPI DSI DTs: short packets have top two bits = 00b or 01b, long
	 * packets have top two bits = 10b or 11b. This matches
	 * mipi_dsi_packet_format_is_long() in the kernel.
	 */
	switch (dt) {
	case 0x09:  /* generic long */
	case 0x19:
	case 0x29:
	case 0x39:  /* DCS long */
	case 0x0e:  /* packed pixel 16 */
	case 0x1e:  /* packed pixel 18 */
	case 0x2e:  /* loosely packed 18 */
	case 0x3e:  /* packed pixel 24 */
		return 1;
	default:
		return 0;
	}
}

/*
 * Command transmission mode.  Default is HS (panel-cwu50.c does not set
 * MIPI_DSI_MODE_LPM so upstream sends init commands in HS) -- but that
 * inference has never been proven on this hardware.  The LP switch lets
 * fbd retry the whole init table in low-power escape mode when the
 * panel demonstrably ignores the HS waveform (no BTA response).
 */
static int _cmd_lp = 0;

void uc_dsi_set_cmd_lp(int on) {
	_cmd_lp = on;
}

int uc_dsi_dcs_write(uint8_t data_type, const uint8_t* payload, uint32_t len) {
	uint32_t pkth = 0;
	uint32_t pktc = 0;
	uint32_t cmd_fifo_len = 0;
	uint32_t i;
	int spin;
	int is_long = _mipi_is_long(data_type);

	uc_dsi_init();
	if (_dsi1 == 0) {
		return -1;
	}

	if (is_long) {
		/*
		 * Panel init commands are all small (≤~16 bytes), so route
		 * everything through the byte-oriented command FIFO. That
		 * avoids having to marshal 4-byte-aligned words into the
		 * pixel FIFO for this phase.
		 */
		cmd_fifo_len = len;
		pkth = ((uint32_t)data_type << TXPKT1H_BC_DT_SHIFT) |
		       (((uint32_t)len & 0xffffU) << TXPKT1H_BC_PARAM_SHIFT) |
		       ((cmd_fifo_len & 0xffU) << TXPKT1H_BC_CMDFIFO_SHIFT);
		pktc |= TXPKT1C_CMD_TYPE_LONG;
		for (i = 0; i < cmd_fifo_len; i++) {
			uc_dsi1_write(UC_DSI1_TXPKT_CMD_FIFO, payload[i]);
		}
	} else {
		uint32_t p0 = (len > 0 && payload) ? payload[0] : 0;
		uint32_t p1 = (len > 1 && payload) ? payload[1] : 0;
		pkth = ((uint32_t)data_type << TXPKT1H_BC_DT_SHIFT) |
		       ((p0 | (p1 << 8)) << TXPKT1H_BC_PARAM_SHIFT);
	}

	/*
	 * HS command mode, NOT LP.  panel-cwu50.c does not set
	 * MIPI_DSI_MODE_LPM, so mipi_dsi_dcs_write_buffer() never passes
	 * MIPI_DSI_MSG_USE_LPM and vc4_dsi_host_transfer() leaves
	 * TXPKT1C_CMD_MODE_LP clear: every cwu50 init command goes out
	 * over the high-speed lanes.  Forcing LP here puts a completely
	 * different waveform on the wire and the panel never latches a
	 * single command (black panel, backlight on).
	 */
	pktc |= TXPKT1C_CMD_CTRL_TX;
	pktc |= (1U << TXPKT1C_CMD_REPEAT_SHIFT);
	pktc |= TXPKT1C_CMD_EN;
	pktc |= TXPKT1C_DISPLAY_NO_SHORT;
	if (_cmd_lp) {
		/* LP retry path: matches MIPI_DSI_MSG_USE_LPM upstream. */
		pktc |= TXPKT1C_CMD_MODE_LP;
	}

	/*
	 * vc4_dsi_host_transfer() enables the TXPKT1_DONE interrupt in
	 * INT_EN before every transfer (on top of the always-enabled
	 * error set written at bind time).  INT_STAT reporting can be
	 * gated by INT_EN on this block, so without this write a
	 * successfully transmitted packet may never show DONE in
	 * INT_STAT.  We poll rather than take IRQs, but the enable is
	 * still required.
	 */
	uc_dsi1_write(UC_DSI1_INT_EN,
			UC_DSI1_INT_ALWAYS_ENABLED | UC_DSI1_INT_TXPKT1_DONE);
	/* Clear stale completion state in both status registers. */
	uc_dsi1_write(UC_DSI1_INT_STAT, UC_DSI1_INT_TXPKT1_DONE);
	uc_dsi1_write(UC_DSI1_STAT, UC_DSI1_STAT_TXPKT1_DONE);

	uc_dsi1_write(UC_DSI1_TXPKT1H, pkth);
	uc_dsi1_write(UC_DSI1_TXPKT1C, pktc);

	/*
	 * Poll ~200 ms.  Accept completion from either INT_STAT or the
	 * raw (non-gated) STAT copy of TXPKT1_DONE.
	 */
	for (spin = 0; spin < 200000; spin++) {
		if ((uc_dsi1_read(UC_DSI1_INT_STAT) & UC_DSI1_INT_TXPKT1_DONE) ||
		    (uc_dsi1_read(UC_DSI1_STAT) & UC_DSI1_STAT_TXPKT1_DONE)) {
			uc_dsi1_write(UC_DSI1_INT_STAT, UC_DSI1_INT_TXPKT1_DONE);
			uc_dsi1_write(UC_DSI1_STAT, UC_DSI1_STAT_TXPKT1_DONE);
			uc_dsi1_write(UC_DSI1_INT_EN, UC_DSI1_INT_ALWAYS_ENABLED);
			return 0;
		}
		uc_udelay(1);
	}

	/* Reset the transmit FIFO the same way vc4 does on error. */
	uc_dsi1_write(UC_DSI1_TXPKT1C, uc_dsi1_read(UC_DSI1_TXPKT1C) & ~TXPKT1C_CMD_EN);
	uc_udelay(1);
	uc_dsi1_write(UC_DSI1_CTRL,
			uc_dsi1_read(UC_DSI1_CTRL) | DSI1_CTRL_RESET_FIFOS);
	uc_dsi1_write(UC_DSI1_TXPKT1C, 0);
	uc_dsi1_write(UC_DSI1_INT_EN, UC_DSI1_INT_ALWAYS_ENABLED);
	return -1;
}

/*
 * DCS read, mirroring vc4_dsi_host_transfer()'s rx path: send the DCS
 * read header with CMD_CTRL_RX (BTA follows the command), wait for
 * PHY_DIR_RTF (bus returned to forward direction after the panel's
 * response), then parse RXPKT1H.  A short response carries up to two
 * bytes inside the header itself; a long response is drained from
 * RXPKT_FIFO.  Returns the number of bytes read, or -1.
 *
 * This is the only probe that proves the PANEL side of the link: a
 * TXPKT1_DONE only shows our controller finished serialising.
 */
int uc_dsi_dcs_read(uint8_t cmd, uint8_t* rx, uint32_t rx_len, int lp) {
	uint32_t pkth, pktc, rxpkt1h;
	uint32_t i;
	int spin;

	uc_dsi_init();
	if (_dsi1 == 0 || rx == 0 || rx_len == 0) {
		return -1;
	}

	/* DCS read, no parameters: DT 0x06, param = cmd. */
	pkth = (0x06U << TXPKT1H_BC_DT_SHIFT) |
	       ((uint32_t)cmd << TXPKT1H_BC_PARAM_SHIFT);
	pktc = TXPKT1C_CMD_CTRL_RX |
	       (1U << TXPKT1C_CMD_REPEAT_SHIFT) |
	       TXPKT1C_DISPLAY_NO_SHORT |
	       TXPKT1C_CMD_EN;
	if (lp) {
		/* Send the read request in LP escape mode (LPDT). */
		pktc |= TXPKT1C_CMD_MODE_LP;
	}

	/* Drain any stale rx state. */
	uc_dsi1_write(UC_DSI1_CTRL,
			uc_dsi1_read(UC_DSI1_CTRL) | DSI1_CTRL_CLR_RXF);

	/*
	 * rx transfers complete on PHY_DIR_RTF, not TXPKT1_DONE.  Also
	 * watch PHY_DIR_FTR (forward->reverse turnaround) so a timeout
	 * can be classified: FTR never set = the controller never even
	 * granted the bus (command/BTA not serialised); FTR set but RTF
	 * missing = the bus was handed to the panel and the panel stayed
	 * silent (link-level / analog problem).
	 */
	uc_dsi1_write(UC_DSI1_INT_STAT,
			UC_DSI1_INT_TXPKT1_DONE | UC_DSI1_INT_PHY_DIR_RTF |
			UC_DSI1_INT_PHY_DIR_FTR);
	uc_dsi1_write(UC_DSI1_INT_EN,
			UC_DSI1_INT_ALWAYS_ENABLED | UC_DSI1_INT_PHY_DIR_RTF |
			UC_DSI1_INT_PHY_DIR_FTR);

	uc_dsi1_write(UC_DSI1_TXPKT1H, pkth);
	uc_dsi1_write(UC_DSI1_TXPKT1C, pktc);

	for (spin = 0; spin < 200000; spin++) {
		if (uc_dsi1_read(UC_DSI1_INT_STAT) & UC_DSI1_INT_PHY_DIR_RTF) {
			break;
		}
		uc_udelay(1);
	}
	uc_dsi1_write(UC_DSI1_INT_EN, UC_DSI1_INT_ALWAYS_ENABLED);
	if (spin >= 200000) {
		int ftr = (uc_dsi1_read(UC_DSI1_INT_STAT) &
				UC_DSI1_INT_PHY_DIR_FTR) != 0;
		/* No response: reset the FIFOs the same way vc4 does. */
		uc_dsi1_write(UC_DSI1_TXPKT1C,
				uc_dsi1_read(UC_DSI1_TXPKT1C) & ~TXPKT1C_CMD_EN);
		uc_udelay(1);
		uc_dsi1_write(UC_DSI1_CTRL,
				uc_dsi1_read(UC_DSI1_CTRL) | DSI1_CTRL_RESET_FIFOS);
		uc_dsi1_write(UC_DSI1_TXPKT1C, 0);
		/*
		 * -1: the BTA was never granted (controller-side issue or
		 *     the panel jammed the LP lines).
		 * -2: turnaround happened, panel just never drove a reply
		 *     (or does not implement BTA reads at all).
		 */
		return ftr ? -2 : -1;
	}

	rxpkt1h = uc_dsi1_read(UC_DSI1_RXPKT1H);
	if (rxpkt1h & RXPKT1H_PKT_TYPE_LONG) {
		uint32_t n = (rxpkt1h >> RXPKT1H_BC_PARAM_SHIFT) &
			     RXPKT1H_BC_PARAM_MASK;
		if (n > rx_len) n = rx_len;
		for (i = 0; i < n; i++) {
			rx[i] = (uint8_t)uc_dsi1_read(UC_DSI1_RXPKT_FIFO);
		}
		return (int)n;
	}
	rx[0] = (uint8_t)(rxpkt1h >> RXPKT1H_SHORT_0_SHIFT);
	if (rx_len > 1) {
		rx[1] = (uint8_t)(rxpkt1h >> RXPKT1H_SHORT_1_SHIFT);
		return 2;
	}
	return 1;
}

void uc_dsi_video_mode(void) {
	uint32_t v;

	uc_dsi_init();
	if (_dsi1 == 0) {
		return;
	}

	/*
	 * vc4_dsi_encoder_enable() for MIPI_DSI_MODE_VIDEO, verbatim: the
	 * whole video-mode DISP0_CTRL — PIX_CLK_DIV, PFORMAT,
	 * LP_STOP_PERFRAME, ST_END and ENABLE — is one single write, done
	 * only after the panel DCS init.  DISP0_CTRL was 0 until now.
	 */
	v = ((uint32_t)UC_DSI_PIXEL_DIVIDER << 13) |
	    ((uint32_t)UC_DSI_FORMAT_RGB888 << 2) |
	    (2U << 11) |          /* LP_STOP_CTRL = LP_STOP_PERFRAME */
	    (1U << 4) |           /* ST_END */
	    DISP0_ENABLE;
	uc_dsi1_write(UC_DSI1_DISP0_CTRL, v);
}
