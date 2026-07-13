#include "uc_dsi.h"
#include "uc_time.h"

#include <stdint.h>

#include <ewoksys/mmio.h>

/* Silent operation -- slog is stubbed to a no-op. */
#define slog(...) ((void)0)

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
	uc_dsi_init();
	if (_dsi1 == 0) {
		slog("[uc_dsi] DSI1 not mapped\n");
		return;
	}

	slog("[uc_dsi] ID          = 0x%08x\n", uc_dsi1_read(UC_DSI1_ID));
	slog("[uc_dsi] CTRL        = 0x%08x\n", uc_dsi1_read(UC_DSI1_CTRL));
	slog("[uc_dsi] STAT        = 0x%08x\n", uc_dsi1_read(UC_DSI1_STAT));
	slog("[uc_dsi] INT_STAT    = 0x%08x\n", uc_dsi1_read(UC_DSI1_INT_STAT));
	slog("[uc_dsi] INT_EN      = 0x%08x\n", uc_dsi1_read(UC_DSI1_INT_EN));
	slog("[uc_dsi] PHYC        = 0x%08x\n", uc_dsi1_read(UC_DSI1_PHYC));
	slog("[uc_dsi] PHY_AFEC0   = 0x%08x\n", uc_dsi1_read(UC_DSI1_PHY_AFEC0));
	slog("[uc_dsi] PHY_AFEC1   = 0x%08x\n", uc_dsi1_read(UC_DSI1_PHY_AFEC1));
	slog("[uc_dsi] DISP0_CTRL  = 0x%08x\n", uc_dsi1_read(UC_DSI1_DISP0_CTRL));
	slog("[uc_dsi] DISP1_CTRL  = 0x%08x\n", uc_dsi1_read(UC_DSI1_DISP1_CTRL));
	slog("[uc_dsi] HS_CLT0     = 0x%08x\n", uc_dsi1_read(UC_DSI1_HS_CLT0));
	slog("[uc_dsi] HS_CLT1     = 0x%08x\n", uc_dsi1_read(UC_DSI1_HS_CLT1));
	slog("[uc_dsi] HS_CLT2     = 0x%08x\n", uc_dsi1_read(UC_DSI1_HS_CLT2));
	slog("[uc_dsi] HS_DLT3     = 0x%08x\n", uc_dsi1_read(UC_DSI1_HS_DLT3));
	slog("[uc_dsi] HS_DLT4     = 0x%08x\n", uc_dsi1_read(UC_DSI1_HS_DLT4));
	slog("[uc_dsi] HS_DLT5     = 0x%08x\n", uc_dsi1_read(UC_DSI1_HS_DLT5));
	slog("[uc_dsi] HS_DLT6     = 0x%08x\n", uc_dsi1_read(UC_DSI1_HS_DLT6));
	slog("[uc_dsi] HS_DLT7     = 0x%08x\n", uc_dsi1_read(UC_DSI1_HS_DLT7));
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
#define TXPKT1C_CMD_REPEAT_SHIFT         10
#define TXPKT1C_DISPLAY_NO_SHORT         (0U << 8)

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

	uc_dsi_init();
	if (_dsi1 == 0) {
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
	slog("[uc_dsi] ULPS %s timeout: STAT=0x%08x\n",
			enter ? "enter" : "exit", uc_dsi1_read(UC_DSI1_STAT));
}

int uc_dsi_bringup(void) {
	uint32_t ui_ns;
	uint32_t lpx;
	uint32_t afec0;

	uc_dsi_init();
	if (_dsi1 == 0) {
		return -1;
	}

	slog("[uc_dsi] bringup: DSI1 ID=0x%08x lanes=%u hs=%uHz\n",
			uc_dsi1_read(UC_DSI1_ID), UC_DSI_LANES, UC_DSI_HS_CLOCK_HZ);

	ui_ns = _ui_ns();
	lpx = _est(60);   /* Minimum LP state = 60ns => 6 escape ticks. */

	/* Reset the controller + all FIFOs. */
	uc_dsi1_write(UC_DSI1_CTRL, DSI1_CTRL_SOFT_RESET_CFG | DSI1_CTRL_RESET_FIFOS);
	uc_dsi1_write(UC_DSI1_CTRL, DSI1_CTRL_HSDT_EOT_DISABLE | DSI1_CTRL_RX_LPDT_EOT_DISABLE);

	/* Clear all STAT bits (W1C). */
	uc_dsi1_write(UC_DSI1_STAT, uc_dsi1_read(UC_DSI1_STAT));

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

	/* Leave ULPS so the panel sees line activity. */
	uc_dsi_ulps(0);

	/*
	 * Match vc4_dsi_bridge_pre_enable() for MIPI_DSI_MODE_VIDEO panels:
	 * program the video-mode DISP0_CTRL fields (PIX_CLK_DIV, PFORMAT,
	 * LP_STOP_PERFRAME, ST_END) but *without* setting DISP0_ENABLE.
	 * DCS transfers do not require DISP0 to be enabled; the ENABLE bit
	 * is OR'd in by uc_dsi_video_mode() after the panel DCS init has
	 * completed, matching upstream vc4_dsi_bridge_enable().
	 */
	uc_dsi1_write(UC_DSI1_DISP0_CTRL,
			((uint32_t)UC_DSI_PIXEL_DIVIDER << 13) |
			((uint32_t)UC_DSI_FORMAT_RGB888 << 2) |
			((uint32_t)2 << 11) |
			(1U << 4));

	slog("[uc_dsi] bringup done: CTRL=0x%08x STAT=0x%08x PHYC=0x%08x AFEC0=0x%08x\n",
			uc_dsi1_read(UC_DSI1_CTRL), uc_dsi1_read(UC_DSI1_STAT),
			uc_dsi1_read(UC_DSI1_PHYC), uc_dsi1_read(UC_DSI1_PHY_AFEC0));
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

	pktc |= TXPKT1C_CMD_MODE_LP;
	pktc |= TXPKT1C_CMD_CTRL_TX;
	pktc |= (1U << TXPKT1C_CMD_REPEAT_SHIFT);
	pktc |= TXPKT1C_CMD_EN;
	pktc |= TXPKT1C_DISPLAY_NO_SHORT;

	/* Clear TXPKT1_DONE before kicking. */
	uc_dsi1_write(UC_DSI1_INT_STAT, UC_DSI1_INT_TXPKT1_DONE);

	uc_dsi1_write(UC_DSI1_TXPKT1H, pkth);
	uc_dsi1_write(UC_DSI1_TXPKT1C, pktc);

	/* Poll ~200 ms. */
	for (spin = 0; spin < 200000; spin++) {
		if (uc_dsi1_read(UC_DSI1_INT_STAT) & UC_DSI1_INT_TXPKT1_DONE) {
			uc_dsi1_write(UC_DSI1_INT_STAT, UC_DSI1_INT_TXPKT1_DONE);
			return 0;
		}
		uc_udelay(1);
	}

	slog("[uc_dsi] dcs_write dt=0x%02x len=%u TIMEOUT INT_STAT=0x%08x STAT=0x%08x\n",
			data_type, len,
			uc_dsi1_read(UC_DSI1_INT_STAT), uc_dsi1_read(UC_DSI1_STAT));

	/* Reset the transmit FIFO the same way vc4 does on error. */
	uc_dsi1_write(UC_DSI1_TXPKT1C, uc_dsi1_read(UC_DSI1_TXPKT1C) & ~TXPKT1C_CMD_EN);
	uc_udelay(1);
	uc_dsi1_write(UC_DSI1_CTRL,
			uc_dsi1_read(UC_DSI1_CTRL) | DSI1_CTRL_RESET_FIFOS);
	uc_dsi1_write(UC_DSI1_TXPKT1C, 0);
	return -1;
}

void uc_dsi_video_mode(void) {
	uint32_t v;

	uc_dsi_init();
	if (_dsi1 == 0) {
		return;
	}

	/*
	 * Matches vc4_dsi_bridge_enable(): OR DISP0_ENABLE into the
	 * already-programmed video-mode DISP0_CTRL. uc_dsi_bringup() left
	 * PIX_CLK_DIV / PFORMAT / LP_STOP_PERFRAME / ST_END set with ENABLE
	 * clear; enabling here starts the controller consuming pixels from
	 * the HVS/PV path.
	 */
	v = uc_dsi1_read(UC_DSI1_DISP0_CTRL) | DISP0_ENABLE;
	uc_dsi1_write(UC_DSI1_DISP0_CTRL, v);

	slog("[uc_dsi] video_mode: DISP0=0x%08x STAT=0x%08x\n",
			uc_dsi1_read(UC_DSI1_DISP0_CTRL),
			uc_dsi1_read(UC_DSI1_STAT));
}
