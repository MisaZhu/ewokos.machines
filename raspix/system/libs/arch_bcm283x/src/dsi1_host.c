#include "dsi1_internal.h"

#include <stdio.h>
#include <stdint.h>

#include <ewoksys/mmio.h>

/*
 * DSI controller + analog PHY bring-up for both ports.  Register
 * offsets from Linux vc4_dsi.c; each port has its own layout (DSI0 is
 * the older single-block design, DSI1 the 4-lane one) but the bring-up
 * sequence is identical.  Identical on BCM2835/2837 (gen4) and BCM2711
 * (gen5).
 */

/* Register index table. */
enum {
	R_CTRL = 0, R_TXPKT1C, R_TXPKT1H, R_CMD_FIFO, R_PIX_FIFO,
	R_DISP0, R_DISP1, R_INT_STAT, R_INT_EN, R_STAT,
	R_HSTX_TO, R_LPRX_TO, R_TA_TO, R_PR_TO,
	R_PHYC, R_CLT0, R_CLT1, R_CLT2,
	R_DLT3, R_DLT4, R_DLT5, R_DLT6, R_DLT7,
	R_AFEC0, R_AFEC1, R_ID, R_COUNT
};

static const uint16_t _regs[2][R_COUNT] = {
	{ /* DSI0 */
		0x00, 0x04, 0x08, 0x14, 0x20,
		0x18, 0x1c, 0x24, 0x28, 0x2c,
		0x30, 0x34, 0x38, 0x3c,
		0x40, 0x44, 0x48, 0x4c,
		0x50, 0x54, 0x58, 0x5c, 0x60,
		0x64, 0x68, 0x74
	},
	{ /* DSI1 */
		0x00, 0x04, 0x08, 0x1c, 0x20,
		0x28, 0x2c, 0x30, 0x34, 0x38,
		0x3c, 0x40, 0x44, 0x48,
		0x4c, 0x50, 0x54, 0x58,
		0x5c, 0x60, 0x64, 0x68, 0x6c,
		0x70, 0x74, 0x8c
	}
};

static uint32_t R(int r) {
	return _regs[dsi1_port()][r];
}

/* ---------- per-port bit layouts ---------- */

/* CTRL: EN is bit 0 on both (DSI0 names it CTRL0); FIFO-reset bits
 * differ, and DSI0's reset value includes CTRL0 itself — the block
 * must be enabled for the FIFO resets to trigger (vc4_dsi.c). */
#define DSI_CTRL_EN                 (1U << 0)
#define DSI_CTRL_CAL_BYTE           (1U << 9)
#define DSI_CTRL_SOFT_RESET_CFG     (1U << 10)
#define DSI_CTRL_HSDT_EOT_DISABLE   (1U << 11)
#define DSI_CTRL_RX_LPDT_EOT_DISABLE (1U << 13)
#define DSI1_CTRL_RESET_FIFOS  ((1U << 7) | (1U << 6) | (1U << 5) | (1U << 4))
#define DSI0_CTRL_RESET_FIFOS  ((1U << 7) | (1U << 6) | (1U << 5) | \
				(1U << 4) | (1U << 3) | (1U << 0))

static uint32_t _ctrl_reset_fifos(void) {
	return dsi1_port() ? DSI1_CTRL_RESET_FIFOS : DSI0_CTRL_RESET_FIFOS;
}

/* PHYC: data-lane enable bits are 4*lane on both ports (DSI0 wires
 * only lanes 0/1); clock-lane enable and HS-continuous differ. */
static uint32_t _phyc_clane_en(void) {
	return dsi1_port() ? (1U << 16) : (1U << 8);
}
static uint32_t _phyc_hs_cont(void) {
	return dsi1_port() ? (1U << 18) : (1U << 10);
}
static uint32_t _phyc_lpdt_shift(void) {
	return dsi1_port() ? 20 : 12;
}

/* AFEC0: RESET/PD bits and the IDR location differ.  DSI0 carries the
 * IDR currents in AFEC1 instead and has a single data-lane PD bit. */
static uint32_t _afec0_reset(void) {
	return dsi1_port() ? (1U << 13) : (1U << 11);
}
static uint32_t _afec0_latch_ulps(void) {
	return dsi1_port() ? (1U << 14) : (1U << 24);
}
static uint32_t _afec0_pd_lane(uint32_t lane) {
	if (dsi1_port()) {
		/* DSI1: PD_DLANE3=8, PD_DLANE2=9, PD_DLANE1=10 */
		return (lane >= 1 && lane <= 3) ?
				(1U << (10 - (lane - 1))) : 0;
	}
	return (lane == 1) ? (1U << 8) : 0;
}

/* STAT lane-STOP reporting bits. */
static uint32_t _stat_stop_bit(uint32_t lane) {
	if (dsi1_port()) {
		return 1U << (24 + 2 * lane);
	}
	/* DSI0 mirrors its INT layout: D0_STOP=14, D1_STOP=20. */
	return (lane == 0) ? (1U << 14) : (1U << 20);
}

/* TXPKT1 completion bits. */
static uint32_t _int_txpkt1_done(void) {
	return dsi1_port() ? (1U << 1) : (1U << 0);
}
static uint32_t _stat_txpkt1_done(void) {
	return dsi1_port() ? (1U << 1) : (1U << 0);
}

/* vc4_dsi INTERRUPTS_ALWAYS_ENABLED, per port. */
static uint32_t _int_always_enabled(void) {
	if (dsi1_port()) {
		return (1U << 6) | (1U << 7) | (1U << 8) | (1U << 9) |
		       (1U << 10) | (1U << 11) | (1U << 12) | (1U << 13);
	}
	return (1U << 3) | (1U << 4) | (1U << 5) | (1U << 6) |
	       (1U << 7) | (1U << 8) | (1U << 9) | (1U << 10);
}

/* HS_CLT0..HS_DLT7 field shifts (identical both ports). */
#define HS_CLT0_CZERO_SHIFT              18
#define HS_CLT0_CPRE_SHIFT               9
#define HS_CLT0_CPREP_SHIFT              0
#define HS_CLT1_CTRAIL_SHIFT             9
#define HS_CLT1_CPOST_SHIFT              0
#define HS_CLT2_WUP_SHIFT                0
#define HS_DLT3_EXIT_SHIFT               18
#define HS_DLT3_ZERO_SHIFT               9
#define HS_DLT3_PRE_SHIFT                0
#define HS_DLT4_ANLAT_SHIFT              18
#define HS_DLT4_TRAIL_SHIFT              9
#define HS_DLT4_LPX_SHIFT                0
#define HS_DLT5_INIT_SHIFT               0
#define HS_DLT6_TA_GET_SHIFT             24
#define HS_DLT6_TA_SURE_SHIFT            16
#define HS_DLT6_TA_GO_SHIFT              8
#define HS_DLT6_LP_LPX_SHIFT             0
#define HS_DLT7_LP_WUP_SHIFT             0

/* DISP1_CTRL. */
#define DISP1_PFORMAT_SHIFT          1
#define DISP1_PFORMAT_32BIT_LE       2U
#define DISP1_ENABLE                 (1U << 0)

/* DISP0_CTRL. */
#define DISP0_ENABLE                 (1U << 0)
#define DISP0_FORMAT_RGB888          3U      /* DSI_DISP0_PFORMAT_RGB888 */

/* TXPKT1C fields. */
#define TXPKT1C_CMD_EN                   (1U << 0)
#define TXPKT1C_CMD_TYPE_LONG            (1U << 2)
#define TXPKT1C_CMD_MODE_LP              (1U << 3)
#define TXPKT1C_CMD_CTRL_TX              (0U << 4)
#define TXPKT1C_CMD_REPEAT_SHIFT         10
#define TXPKT1C_DISPLAY_NO_SHORT         (0U << 8)
#define TXPKT1C_DISPLAY_NO_SECONDARY     (2U << 8)

/* TXPKT1H fields. */
#define TXPKT1H_BC_DT_SHIFT              0
#define TXPKT1H_BC_PARAM_SHIFT           8
#define TXPKT1H_BC_CMDFIFO_SHIFT         24

/*
 * Lanes + HS bit clock handed to host_bringup(); kept module-static so
 * the HS timing helpers can use them.
 */
static uint32_t _lanes = 2;
static uint32_t _hs_clock_hz = 0;

/* Firmware snapshot buffer (filled by dump_firmware at startup). */
static char _fw_snap[640];

/*
 * Unit-interval-in-ns for the target HS bit clock.  The PHY clock is
 * DDR, so 1 UI = 2 clock periods, hence 500e6/hz (vc4_dsi.c).
 */
static uint32_t _ui_ns(void) {
	if (_hs_clock_hz == 0) {
		return 0;
	}
	return (500000000U + _hs_clock_hz - 1U) / _hs_clock_hz;
}

/* From vc4_dsi.c::dsi_hs_timing().  Round up to a multiple of 8
 * (byte clock). */
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

static uint32_t _stat_stop_mask(void) {
	uint32_t m = _stat_stop_bit(0);
	if (_lanes > 1) m |= _stat_stop_bit(1);
	if (_lanes > 2) m |= _stat_stop_bit(2);
	if (_lanes > 3) m |= _stat_stop_bit(3);
	return m;
}

/*
 * Same liveness check vc4_dsi's probe does: the ID register must read
 * 0x00647369 ("dsi").  Note this alone cannot prove the port is
 * usable — a powered-off port still reads the ID but drops writes
 * (use bcm283x_dsi1_probe_port() for that).
 */
int bcm283x_dsi1_alive(void) {
	if (_mmio_base == 0) {
		return -1;
	}
	return (dsi1_dsi_read(R(R_ID)) == 0x00647369U) ? 0 : -1;
}

/*
 * After bring-up all enabled data lanes must sit in LP-11 STOP; that is
 * the analog PHY actually driving the lines.  The AFE needs a little
 * settle time after AFEC RESET release, so poll instead of doing a
 * single instant read (upstream never checks at all).  100 ms budget,
 * early exit.
 */
int bcm283x_dsi1_lanes_stopped(void) {
	uint32_t stat_stop = _stat_stop_mask();
	int spin;

	for (spin = 0; spin < 10000; spin++) {
		if ((dsi1_dsi_read(R(R_STAT)) & stat_stop) == stat_stop) {
			return 0;
		}
		bcm283x_dsi1_udelay(10);
	}
	return -1;
}

int bcm283x_dsi1_host_bringup(uint32_t lanes, uint32_t hs_clock_hz,
		int continuous_clock) {
	uint32_t ui_ns;
	uint32_t lpx;
	uint32_t afec0;
	uint32_t phyc;

	if (lanes == 0 || hs_clock_hz == 0) {
		return -1;
	}
	/* DSI0's PHY wires two data lanes; DSI1 four. */
	if (lanes > (dsi1_port() ? 4U : 2U)) {
		return -1;
	}
	_lanes = lanes;
	_hs_clock_hz = hs_clock_hz;

	ui_ns = _ui_ns();
	lpx = _est(60);   /* Minimum LP state = 60ns => 6 escape ticks. */

	/* Reset the controller + all FIFOs, then leave the block ENABLED
	 * — vc4 sets EN/CTRL0 right here, not at the end of bring-up. */
	dsi1_dsi_write(R(R_CTRL), DSI_CTRL_SOFT_RESET_CFG | _ctrl_reset_fifos());
	dsi1_dsi_write(R(R_CTRL), DSI_CTRL_EN |
			DSI_CTRL_HSDT_EOT_DISABLE | DSI_CTRL_RX_LPDT_EOT_DISABLE);

	/* Clear all STAT bits (W1C). */
	dsi1_dsi_write(R(R_STAT), dsi1_dsi_read(R(R_STAT)));

	/*
	 * Mirror vc4_dsi bind: keep the error/timeout interrupts enabled
	 * from the start and flush any latched interrupt state.  Some of
	 * the INT_STAT reporting is gated by INT_EN, so leaving INT_EN
	 * at 0 hides transfer completion from the polling loop.
	 */
	dsi1_dsi_write(R(R_INT_EN), _int_always_enabled());
	dsi1_dsi_write(R(R_INT_STAT), dsi1_dsi_read(R(R_INT_STAT)));

	/*
	 * Bring the analog PHY out of powerdown.  IDR values of 6 for
	 * all lanes match vc4's DSI1 path; on DSI0 the currents live in
	 * AFEC1 (clane/d0/d1 at 3-bit fields 0/4/8).
	 *
	 * AFEC0 RESET handling differs per port (vc4_dsi.c): DSI1
	 * always comes up with RESET held and released after the HS
	 * timing/PHYC setup; DSI0 only holds RESET for command-mode
	 * panels — a VIDEO panel's AFE must come up running, so on
	 * DSI0 we never assert it (we only drive video panels).
	 */
	afec0 = (7U << 4) | 7U;   /* PTATADJ | CTATADJ */
	if (dsi1_port()) {
		afec0 |= (6U << 17) | (6U << 20) | (6U << 23) |
			 (6U << 26) | (6U << 29);
	}
	if (_lanes < 4) afec0 |= _afec0_pd_lane(3);
	if (_lanes < 3) afec0 |= _afec0_pd_lane(2);
	if (_lanes < 2) afec0 |= _afec0_pd_lane(1);
	if (dsi1_port()) {
		afec0 |= _afec0_reset();
		dsi1_dsi_write(R(R_AFEC0), afec0);
		dsi1_dsi_write(R(R_AFEC1), 0);
		bcm283x_dsi1_mdelay(1);
	} else {
		dsi1_dsi_write(R(R_AFEC0), afec0);
		bcm283x_dsi1_mdelay(1);
		dsi1_dsi_write(R(R_AFEC1), (6U << 0) | (6U << 4) | (6U << 8));
	}

	/* HS timing regs — verbatim from vc4_dsi_encoder_enable(). */
	dsi1_dsi_write(R(R_CLT0),
			(_hst(ui_ns, 262, 0) << HS_CLT0_CZERO_SHIFT) |
			(_hst(ui_ns, 0,   8) << HS_CLT0_CPRE_SHIFT)  |
			(_hst(ui_ns, 38,  0) << HS_CLT0_CPREP_SHIFT));
	dsi1_dsi_write(R(R_CLT1),
			(_hst(ui_ns, 60, 0) << HS_CLT1_CTRAIL_SHIFT) |
			(_hst(ui_ns, 60, 52) << HS_CLT1_CPOST_SHIFT));
	dsi1_dsi_write(R(R_CLT2),
			(_hst(ui_ns, 1000000, 0) << HS_CLT2_WUP_SHIFT));
	dsi1_dsi_write(R(R_DLT3),
			(_hst(ui_ns, 100, 0) << HS_DLT3_EXIT_SHIFT) |
			(_hst(ui_ns, 105, 6) << HS_DLT3_ZERO_SHIFT) |
			(_hst(ui_ns, 40,  4) << HS_DLT3_PRE_SHIFT));
	dsi1_dsi_write(R(R_DLT4),
			(_hst(ui_ns, lpx * 10, 0) << HS_DLT4_LPX_SHIFT) |
			(_hst_max(_hst(ui_ns, 0, 8),
				  _hst(ui_ns, 60, 4)) << HS_DLT4_TRAIL_SHIFT) |
			(0U << HS_DLT4_ANLAT_SHIFT));
	dsi1_dsi_write(R(R_DLT5),
			(_hst(ui_ns, 5 * 1000 * 1000, 0) << HS_DLT5_INIT_SHIFT));
	dsi1_dsi_write(R(R_DLT6),
			((lpx * 5) << HS_DLT6_TA_GET_SHIFT) |
			(lpx       << HS_DLT6_TA_SURE_SHIFT) |
			((lpx * 4) << HS_DLT6_TA_GO_SHIFT) |
			(lpx       << HS_DLT6_LP_LPX_SHIFT));
	dsi1_dsi_write(R(R_DLT7),
			(_est(1000000) << HS_DLT7_LP_WUP_SHIFT));

	/*
	 * PHYC: enable clock lane + data lanes.  HS_CLK_CONTINUOUS is
	 * only set for panels that want an always-running HS clock;
	 * MIPI_DSI_CLOCK_NON_CONTINUOUS panels (e.g. the Waveshare 4")
	 * get LP-11 between HS bursts.
	 */
	phyc = (1U << 0) |
			(_lanes >= 2 ? (1U << 4) : 0) |
			(_lanes >= 3 ? (1U << 8) : 0) |
			(_lanes >= 4 ? (1U << 12) : 0) |
			_phyc_clane_en() |
			(continuous_clock ? _phyc_hs_cont() : 0) |
			(((lpx - 1U) << _phyc_lpdt_shift()) & (0x3fU << _phyc_lpdt_shift()));
	dsi1_dsi_write(R(R_PHYC), phyc);

	/* Byte calibration. */
	dsi1_dsi_write(R(R_CTRL),
			dsi1_dsi_read(R(R_CTRL)) | DSI_CTRL_CAL_BYTE);

	/* Timeouts (vc4 uses disable / large fixed values). */
	dsi1_dsi_write(R(R_HSTX_TO), 0);
	dsi1_dsi_write(R(R_LPRX_TO), 0xffffffU);
	dsi1_dsi_write(R(R_TA_TO),   100000);
	dsi1_dsi_write(R(R_PR_TO),   100000);

	/*
	 * DISP1 for long command payloads through the pixel FIFO.  The
	 * PFORMAT value 2 means 32BIT_LE on DSI1 (cmd_fifo_width 4) and
	 * 24BIT on DSI0 (cmd_fifo_width 3) — exactly what upstream
	 * selects for each variant.
	 */
	dsi1_dsi_write(R(R_DISP1),
			(DISP1_PFORMAT_32BIT_LE << DISP1_PFORMAT_SHIFT) |
			DISP1_ENABLE);

	/* Release AFEC RESET (no-op on DSI0, where it was never held). */
	dsi1_dsi_write(R(R_AFEC0),
			dsi1_dsi_read(R(R_AFEC0)) & ~_afec0_reset());

	/*
	 * vc4_dsi_ulps(false) at cold boot: upstream checks the
	 * AFEC0 LATCH_ULPS latch and returns early because it is
	 * already clear — a no-op, mirrored here deliberately.
	 */
	if ((dsi1_dsi_read(R(R_AFEC0)) & _afec0_latch_ulps()) == 0) {
		/* nothing to do */
	}

	/*
	 * Upstream leaves DISP0_CTRL at 0 all the way through the panel
	 * init; the complete video-mode value INCLUDING the ENABLE bit
	 * is written in one shot by bcm283x_dsi1_video_mode().
	 */
	return 0;
}

static int _mipi_is_long(uint8_t dt) {
	/*
	 * MIPI DSI DTs: short packets have top two bits = 00b or 01b,
	 * long packets have top two bits = 10b or 11b.  Matches
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

int bcm283x_dsi1_cmd_write(uint8_t data_type, const uint8_t* payload,
		uint32_t len) {
	uint32_t pkth = 0;
	uint32_t pktc = 0;
	uint32_t cmd_fifo_len = 0;
	uint32_t i;
	int spin;
	int is_long = _mipi_is_long(data_type);

	if (is_long) {
		uint32_t pix_fifo_len = 0;
		/* Pixel-FIFO word width: 4 bytes on DSI1, 3 on DSI0. */
		uint32_t w = dsi1_port() ? 4U : 3U;

		/*
		 * vc4_dsi_host_transfer(): payloads up to 16 bytes fit the
		 * byte-oriented command FIFO alone.  Longer payloads keep
		 * the len%w residue — from the START of the payload — in
		 * the command FIFO and stream the rest through the pixel
		 * FIFO (little-endian 32-bit words on DSI1, byte-swapped
		 * 24-bit words on DSI0), with the packet routed to the
		 * "secondary display" datapath.
		 */
		if (len <= 16) {
			cmd_fifo_len = len;
		} else {
			cmd_fifo_len = len % w;
			pix_fifo_len = (len - cmd_fifo_len) / w;
		}
		pkth = ((uint32_t)data_type << TXPKT1H_BC_DT_SHIFT) |
		       (((uint32_t)len & 0xffffU) << TXPKT1H_BC_PARAM_SHIFT) |
		       ((cmd_fifo_len & 0xffU) << TXPKT1H_BC_CMDFIFO_SHIFT);
		pktc |= TXPKT1C_CMD_TYPE_LONG;
		if (pix_fifo_len != 0) {
			pktc |= TXPKT1C_DISPLAY_NO_SECONDARY;
		}
		for (i = 0; i < cmd_fifo_len; i++) {
			dsi1_dsi_write(R(R_CMD_FIFO), payload[i]);
		}
		for (i = 0; i < pix_fifo_len; i++) {
			const uint8_t* pix = payload + cmd_fifo_len + i * w;
			if (w == 4U) {
				dsi1_dsi_write(R(R_PIX_FIFO),
						(uint32_t)pix[0] |
						((uint32_t)pix[1] << 8) |
						((uint32_t)pix[2] << 16) |
						((uint32_t)pix[3] << 24));
			} else {
				dsi1_dsi_write(R(R_PIX_FIFO),
						(uint32_t)pix[2] |
						((uint32_t)pix[1] << 8) |
						((uint32_t)pix[0] << 16));
			}
		}
	} else {
		uint32_t p0 = (len > 0 && payload) ? payload[0] : 0;
		uint32_t p1 = (len > 1 && payload) ? payload[1] : 0;
		pkth = ((uint32_t)data_type << TXPKT1H_BC_DT_SHIFT) |
		       ((p0 | (p1 << 8)) << TXPKT1H_BC_PARAM_SHIFT);
	}

	/* HS command mode (upstream default when the panel does not set
	 * MIPI_DSI_MODE_LPM). */
	pktc |= TXPKT1C_CMD_CTRL_TX;
	pktc |= (1U << TXPKT1C_CMD_REPEAT_SHIFT);
	pktc |= TXPKT1C_CMD_EN;
	pktc |= TXPKT1C_DISPLAY_NO_SHORT;

	/*
	 * vc4_dsi_host_transfer() enables TXPKT1_DONE in INT_EN before
	 * every transfer: INT_STAT reporting can be gated by INT_EN on
	 * this block, so without this write a successfully transmitted
	 * packet may never show DONE in INT_STAT.
	 */
	dsi1_dsi_write(R(R_INT_EN),
			_int_always_enabled() | _int_txpkt1_done());
	/* Clear stale completion state in both status registers. */
	dsi1_dsi_write(R(R_INT_STAT), _int_txpkt1_done());
	dsi1_dsi_write(R(R_STAT), _stat_txpkt1_done());

	dsi1_dsi_write(R(R_TXPKT1H), pkth);
	dsi1_dsi_write(R(R_TXPKT1C), pktc);

	/*
	 * Poll ~200 ms.  Accept completion from either INT_STAT or the
	 * raw (non-gated) STAT copy of TXPKT1_DONE.
	 */
	for (spin = 0; spin < 200000; spin++) {
		if ((dsi1_dsi_read(R(R_INT_STAT)) & _int_txpkt1_done()) ||
		    (dsi1_dsi_read(R(R_STAT)) & _stat_txpkt1_done())) {
			dsi1_dsi_write(R(R_INT_STAT), _int_txpkt1_done());
			dsi1_dsi_write(R(R_STAT), _stat_txpkt1_done());
			dsi1_dsi_write(R(R_INT_EN), _int_always_enabled());
			return 0;
		}
		bcm283x_dsi1_udelay(1);
	}

	/* Reset the transmit FIFO the same way vc4 does on error. */
	dsi1_dsi_write(R(R_TXPKT1C), dsi1_dsi_read(R(R_TXPKT1C)) & ~TXPKT1C_CMD_EN);
	bcm283x_dsi1_udelay(1);
	dsi1_dsi_write(R(R_CTRL),
			dsi1_dsi_read(R(R_CTRL)) | _ctrl_reset_fifos());
	dsi1_dsi_write(R(R_TXPKT1C), 0);
	dsi1_dsi_write(R(R_INT_EN), _int_always_enabled());
	return -1;
}

/*
 * vc4_dsi_encoder_enable() for MIPI_DSI_MODE_VIDEO, verbatim: the whole
 * video-mode DISP0_CTRL — PIX_CLK_DIV, PFORMAT, LP_STOP_PERFRAME,
 * ST_END and ENABLE — is one single write, done only after the panel
 * init.  DISP0_CTRL was 0 until now.
 */
void bcm283x_dsi1_video_mode(uint32_t pix_clk_divider) {
	uint32_t v;

	v = ((pix_clk_divider & 0x7fU) << 13) |   /* DSI_DISP0_PIX_CLK_DIV */
	    ((uint32_t)DISP0_FORMAT_RGB888 << 2) | /* DSI_DISP0_PFORMAT */
	    (2U << 11) |          /* LP_STOP_CTRL = LP_STOP_PERFRAME */
	    (1U << 4) |           /* ST_END */
	    DISP0_ENABLE;
	dsi1_dsi_write(R(R_DISP0), v);
}

/*
 * Post-enable readback: proves the video-mode DISP0 enable and the
 * PV enable bits actually stuck once the whole pipeline is live
 * (frame counter advancing).  DISP0 reading back without its enable
 * bit, or PV V_CONTROL without VIDEN, means the link never started.
 */
void bcm283x_dsi1_dump_live(void) {
	printf("dsi%d: live CTRL=%08x STAT=%08x DISP0=%08x | pv C=%08x V=%08x\n",
			dsi1_port(),
			(unsigned)dsi1_dsi_read(R(R_CTRL)),
			(unsigned)dsi1_dsi_read(R(R_STAT)),
			(unsigned)dsi1_dsi_read(R(R_DISP0)),
			(unsigned)dsi1_pv_read(0x00U),
			(unsigned)dsi1_pv_read(0x04U));
	dsi1_hvs_dump_live();
}

/*
 * Fatal-bringup register dump: every bank that matters for the
 * clock -> PHY -> lanes path on the selected port, plus the port
 * probe verdicts so a dead-port situation is visible immediately.
 */
void bcm283x_dsi1_dump(void) {
	int p = dsi1_port();

	/* Firmware recipe captured at startup (scrolled off by now). */
	if (_fw_snap[0] != '\0') {
		printf("%s", _fw_snap);
	}

	printf("dsi: gen%s xosc=%uHz port=%d probe0=%d probe1=%d\n",
			bcm283x_dsi1_is_gen5() ? "5" : "4",
			(unsigned)dsi1_xosc_hz(), p,
			bcm283x_dsi1_probe_port(0) == 0 ? 1 : 0,
			bcm283x_dsi1_probe_port(1) == 0 ? 1 : 0);
	printf("dsi: CM_LOCK=%08x CM_PLLD=%08x A2W CTRL=%08x FRAC=%08x PER=%08x PLLA_DSI0=%08x PLLD_DSI0=%08x PLLD_DSI1=%08x\n",
			(unsigned)dsi1_cprman_read(0x114U),
			(unsigned)dsi1_cprman_read(0x10cU),
			(unsigned)dsi1_cprman_read(0x1140U),
			(unsigned)dsi1_cprman_read(0x1240U),
			(unsigned)dsi1_cprman_read(0x1540U),
			(unsigned)dsi1_cprman_read(0x1300U),
			(unsigned)dsi1_cprman_read(0x1340U),
			(unsigned)dsi1_cprman_read(0x1640U));
	printf("dsi: CM DSI0 E=%08x P=%08x HSCK=%08x | DSI1 E=%08x P=%08x\n",
			(unsigned)dsi1_cprman_read(0x058U),
			(unsigned)dsi1_cprman_read(0x060U),
			(unsigned)dsi1_cprman_read(0x120U),
			(unsigned)dsi1_cprman_read(0x158U),
			(unsigned)dsi1_cprman_read(0x160U));
	printf("dsi%d: CTRL=%08x STAT=%08x INT_STAT=%08x INT_EN=%08x\n",
			p,
			(unsigned)dsi1_dsi_read(R(R_CTRL)),
			(unsigned)dsi1_dsi_read(R(R_STAT)),
			(unsigned)dsi1_dsi_read(R(R_INT_STAT)),
			(unsigned)dsi1_dsi_read(R(R_INT_EN)));
	printf("dsi%d: PHYC=%08x AFEC0=%08x AFEC1=%08x ID=%08x\n",
			p,
			(unsigned)dsi1_dsi_read(R(R_PHYC)),
			(unsigned)dsi1_dsi_read(R(R_AFEC0)),
			(unsigned)dsi1_dsi_read(R(R_AFEC1)),
			(unsigned)dsi1_dsi_read(R(R_ID)));
	printf("dsi%d: DISP0=%08x DISP1=%08x CLT0=%08x DLT4=%08x\n",
			p,
			(unsigned)dsi1_dsi_read(R(R_DISP0)),
			(unsigned)dsi1_dsi_read(R(R_DISP1)),
			(unsigned)dsi1_dsi_read(R(R_CLT0)),
			(unsigned)dsi1_dsi_read(R(R_DLT4)));
}

/*
 * The VC firmware is still streaming video on this port when the
 * daemon takes over.  Re-programming the PHY under a live HS
 * transmitter produces LP contention (ERR_CONT_LP1) and the lanes can
 * never settle to STOP.  Kill the firmware stream first: DISP0 off,
 * controller gated off, PV off — the link falls to LP-11 and the
 * panel waits for the next T_INIT from our bring-up.
 */
void bcm283x_dsi1_firmware_handoff(void) {
	dsi1_dsi_write(R(R_DISP0), 0);
	dsi1_dsi_write(R(R_CTRL),
			dsi1_dsi_read(R(R_CTRL)) & ~DSI_CTRL_EN);
	dsi1_pv_write(0x00U, dsi1_pv_read(0x00U) & ~1U);
	bcm283x_dsi1_mdelay(20);
}

/*
 * Firmware snapshot: the VC firmware is driving the panel when the
 * daemon starts, so its DSI/PV/clock programming is a known-good
 * recipe for this exact board.  Capture it before anything is
 * overwritten — read-only, both ports, no port routing.  The lines
 * are also kept in a buffer so the fatal dump can reprint them after
 * they have scrolled off the console.
 */
void bcm283x_dsi1_dump_firmware(void) {
	int n = 0;

	n += snprintf(_fw_snap + n, sizeof(_fw_snap) - n,
		"dsi: fw CM DSI0 E=%08x D=%08x P=%08x Q=%08x HSCK=%08x\n",
		(unsigned)dsi1_cprman_read(0x058U),
		(unsigned)dsi1_cprman_read(0x05cU),
		(unsigned)dsi1_cprman_read(0x060U),
		(unsigned)dsi1_cprman_read(0x064U),
		(unsigned)dsi1_cprman_read(0x120U));
	n += snprintf(_fw_snap + n, sizeof(_fw_snap) - n,
		"dsi: fw dsi0 CTRL=%08x PHYC=%08x DISP0=%08x | dsi1 CTRL=%08x DISP0=%08x\n",
		(unsigned)dsi1_dsi_read_port(0, 0x00U),
		(unsigned)dsi1_dsi_read_port(0, 0x04U),
		(unsigned)dsi1_dsi_read_port(0, 0x18U),
		(unsigned)dsi1_dsi_read_port(1, 0x00U),
		(unsigned)dsi1_dsi_read_port(1, 0x18U));
	n += snprintf(_fw_snap + n, sizeof(_fw_snap) - n,
		"dsi: fw pv0 C=%08x HA=%08x HB=%08x VA=%08x VB=%08x ACT=%08x\n",
		(unsigned)dsi1_pv_read_port(0, 0x00U),
		(unsigned)dsi1_pv_read_port(0, 0x0cU),
		(unsigned)dsi1_pv_read_port(0, 0x10U),
		(unsigned)dsi1_pv_read_port(0, 0x14U),
		(unsigned)dsi1_pv_read_port(0, 0x18U),
		(unsigned)dsi1_pv_read_port(0, 0x30U));
	snprintf(_fw_snap + n, sizeof(_fw_snap) - n,
		"dsi: fw pv1 C=%08x HA=%08x HB=%08x VA=%08x VB=%08x ACT=%08x\n",
		(unsigned)dsi1_pv_read_port(1, 0x00U),
		(unsigned)dsi1_pv_read_port(1, 0x0cU),
		(unsigned)dsi1_pv_read_port(1, 0x10U),
		(unsigned)dsi1_pv_read_port(1, 0x14U),
		(unsigned)dsi1_pv_read_port(1, 0x18U),
		(unsigned)dsi1_pv_read_port(1, 0x30U));
	printf("%s", _fw_snap);
}
