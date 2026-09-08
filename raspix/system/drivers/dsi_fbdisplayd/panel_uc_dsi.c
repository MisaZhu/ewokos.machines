#include <stdio.h>
#include <stdint.h>

#include <ewoksys/mmio.h>

#include <arch/bcm283x/dsi1.h>

#include "panel_uc.h"

/*
 * Clock + DSI-host bring-up for the ClockworkPi panels, transcribed
 * line for line from the two files fbdisplay6d actually links:
 *
 *   drivers/dsi/uc_clock.c        :: uc_clock_bringup_dsi1()
 *   drivers/fbdisplay6d/uc_dsi.c  :: uc_dsi_bringup() / _alive() /
 *                                    _lanes_stopped() / _dcs_write() /
 *                                    _video_mode()
 *
 * WHY THIS IS NOT THE SHARED arch LIBRARY
 * ---------------------------------------
 * arch/bcm283x's bcm283x_dsi1_clock_bringup() / _host_bringup() differ
 * from the proven uConsole sequence in four places, all of them inside
 * the stage 2 -> stage 4 window (clocks, then PHY LP-11).  Both were
 * compared register by register; the DSI1 register offsets, PHYC/STAT/
 * AFEC0 bit layouts and every HS_CLT and HS_DLT timing formula are
 * identical, so only these four deviate:
 *
 * A. PLLD VCO.  uc_clock.c computes VCO = XOSC * (NDIV + FDIV/2^20) /
 *    PDIV straight off A2W_PLLD_CTRL/FRAC.  dsi1_clock.c additionally
 *    doubles NDIV and FDIV when A2W_PLLD_ANA1 (ana_reg_base+4) bit 14
 *    (the feedback pre-divider) reads set.  If that bit is set on this
 *    firmware the arch path picks a PLLD_DSI1 channel divider twice as
 *    large, so the PHY really runs at half the HS bit clock while every
 *    HS_CLT and HS_DLT value was computed for the full rate -- the lanes
 *    then never settle into LP-11 STOP.
 *
 * B. DSI1E escape clock.  Both write mux index 6 into CM_DSI1ECTL, but
 *    they disagree about what that node ticks at, and therefore about
 *    the divider:
 *      uc_clock.c   parent = A2W_PLLD_PER rate (VCO / per_div, 750 MHz
 *                   on Pi4) -> div code 7.5
 *      dsi1_clock.c parent = hs_clock / 4 (93.75 MHz for cwu50) -> div
 *                   code 0.9375, which its own `if (code < 1<<12)`
 *                   clamp then raises to 1.0
 *    That is a 7.5x difference in the escape clock.  Every
 *    escape-domain constant (_est(), i.e. LPX, TA_GO/TA_SURE/TA_GET,
 *    LP_WUP and PHYC.ESC_CLK_LPDT) assumes 10 ns per tick, so the two
 *    produce LP waveforms 7.5x apart in real time.  Note dsi1.h's own
 *    contract for bcm283x_dsi1_clock_bringup() says "CM_DSI1ECTL
 *    (escape, 100 MHz from PLLD_PER)" and dsi1_clock.c's fallback
 *    comment says "fall back from PLLD_PER to XOSC" -- the header and
 *    the comment both describe uc_clock.c's behaviour, not the code
 *    that sits under them.
 *
 * C. CTRL EN timing.  uc_dsi_bringup() writes CTRL twice with EN clear
 *    (soft-reset + FIFO flush, then the EOT disables), programs the
 *    whole HS timing bank, PHYC, the four timeout counters and
 *    DISP1_CTRL, and only THEN sets CTRL_EN, immediately before
 *    releasing AFEC0 RESET.  dsi1_host.c ORs EN into its second CTRL
 *    write, so the digital block is gated on while the PHY timing
 *    registers are still being filled in.
 *
 * D. The register write path itself.  uc_dsi.c's uc_dsi1_write() is a
 *    bare `_dsi1[off/4] = val`.  The shared library's dsi1_dsi_write()
 *    routes through _dsi_write_port(), which calls
 *    bcm283x_dsi1_is_gen5() on EVERY store — and that is a
 *    sys_get_sys_info() syscall per DSI register write, several
 *    thousand of them across the bring-up plus the whole DCS table.
 *    Worse, if it ever answers 0 the store is diverted to the gen4
 *    broken-AXI DMA workaround, whose DEST_AD is hardcoded to
 *    0x7e700000 — the BCM2835 bus window, not BCM2711's 0xfe700000 —
 *    so on a CM4 every write silently vanishes while reads keep
 *    working.  That is precisely the observed signature: the ID
 *    register reads back fine, the PHY never reaches LP-11, and the
 *    panel stays deaf with its backlight on.
 *
 * The shared library is left untouched on purpose: it drives the
 * ws/rpi7 families, whose behaviour must not move, and fixing it there
 * would also force a libarch_bcm283x rebuild.  Everything below talks
 * to _mmio_base directly, exactly as the two source files do.
 *
 * Delays use bcm283x_dsi1_udelay/mdelay, which are byte-for-byte the
 * same primitives as uc_time.c's uc_udelay/uc_mdelay (STC CLO at
 * MMIO+0x3004 for the microsecond spin, usleep() for the millisecond
 * one), so every settle interval below is unchanged.
 */

/* ================= CPRMAN (clock/PLL) ================= */

#define UC_CPRMAN_OFFSET        0x101000U
#define UC_CM_PASSWORD          0x5a000000U

#define UC_CM_ENABLE            (1U << 4)
#define UC_CM_KILL              (1U << 5)
#define UC_CM_BUSY              (1U << 7)
#define UC_CM_FRAC              (1U << 9)
#define UC_CM_SRC_MASK          0xfU
#define UC_CM_SRC_OSC           1U
#define UC_CM_SRC_PLLD_PER      6U
#define UC_CM_SRC_DSI1_BYTE     8U

#define UC_DSI_ESC_CLOCK_HZ     100000000U

#define UC_CM_PLLD              0x10cU
#define UC_CM_LOCK              0x114U
#define UC_CM_DSI1ECTL          0x158U
#define UC_CM_DSI1EDIV          0x15cU
#define UC_CM_DSI1PCTL          0x160U
#define UC_CM_DSI1PDIV          0x164U

#define UC_CM_PLLD_HOLDDSI1     (1U << 3)
#define UC_CM_PLLD_LOADDSI1     (1U << 2)

#define UC_CM_LOCK_FLOCKD       (1U << 11)

#define UC_A2W_PLLD_CTRL        0x1140U
#define UC_A2W_PLLD_FRAC        0x1240U
#define UC_A2W_PLLD_PER         0x1540U
#define UC_A2W_PLLD_DSI1        0x1640U

#define UC_A2W_PLL_CTRL_PDIV_SHIFT      12
#define UC_A2W_PLL_CTRL_PDIV_MASK       0x00007000U
#define UC_A2W_PLL_CTRL_NDIV_MASK       0x000003ffU

#define UC_A2W_PLL_CHANNEL_DISABLE      (1U << 8)
#define UC_A2W_PLL_DIV_MASK             0xffU

static volatile uint32_t* _cprman = 0;

static void _cprman_init(void) {
	if (_cprman == 0 && _mmio_base != 0)
		_cprman = (volatile uint32_t*)(uintptr_t)(_mmio_base +
				UC_CPRMAN_OFFSET);
}

static uint32_t _cprman_read(uint32_t off) {
	_cprman_init();
	if (_cprman == 0)
		return 0;
	return _cprman[off / 4];
}

static void _cprman_write(uint32_t off, uint32_t val) {
	_cprman_init();
	if (_cprman == 0)
		return;
	_cprman[off / 4] = UC_CM_PASSWORD | (val & 0x00ffffffU);
}

/*
 * Program the A2W_PLLD_DSI1 integer divider and enable the channel.
 * After the divider is set CM_PLLD_LOADDSI1 is pulsed to latch it.
 */
static void _plld_dsi1_set_divider(uint32_t divider) {
	uint32_t ch;

	/* Hold the DSI1 phase during the update. */
	_cprman_write(UC_CM_PLLD, _cprman_read(UC_CM_PLLD) | UC_CM_PLLD_HOLDDSI1);

	ch = _cprman_read(UC_A2W_PLLD_DSI1);
	ch &= ~UC_A2W_PLL_DIV_MASK;
	ch |= (divider & UC_A2W_PLL_DIV_MASK);
	ch &= ~UC_A2W_PLL_CHANNEL_DISABLE;
	_cprman_write(UC_A2W_PLLD_DSI1, ch);

	/* Pulse LOADDSI1 to latch the new divider. */
	_cprman_write(UC_CM_PLLD, _cprman_read(UC_CM_PLLD) | UC_CM_PLLD_LOADDSI1);
	bcm283x_dsi1_udelay(1);
	_cprman_write(UC_CM_PLLD, _cprman_read(UC_CM_PLLD) & ~UC_CM_PLLD_LOADDSI1);

	/* Release the hold. */
	_cprman_write(UC_CM_PLLD, _cprman_read(UC_CM_PLLD) & ~UC_CM_PLLD_HOLDDSI1);
}

/*
 * CM_*CTL update: KILL the clock, wait for BUSY to clear, rewrite the
 * source + divider, then set ENABLE (clk-bcm2835.c's reparenting
 * pattern).  `div_code` is the raw CM_*DIV value: ALWAYS 12.12 fixed
 * point regardless of the clock's int_bits/frac_bits.  `use_frac`
 * mirrors bcm2835_clock_set_rate — CM_FRAC must be set whenever any
 * fraction bit is non-zero.
 */
static int _cm_set(uint32_t ctl_off, uint32_t div_off,
		uint32_t src, uint32_t div_code, int use_frac, int verify_busy) {
	uint32_t ctl;
	uint32_t ctl_val;
	int spin;

	ctl = _cprman_read(ctl_off);
	if (ctl & UC_CM_ENABLE) {
		_cprman_write(ctl_off, (ctl & ~UC_CM_ENABLE) | UC_CM_KILL);
		for (spin = 0; spin < 10000; spin++) {
			if ((_cprman_read(ctl_off) & UC_CM_BUSY) == 0)
				break;
			bcm283x_dsi1_udelay(1);
		}
		if ((_cprman_read(ctl_off) & UC_CM_BUSY) != 0)
			return -1;
	}

	_cprman_write(div_off, div_code & 0xffffffU);
	ctl_val = (src & UC_CM_SRC_MASK) | (use_frac ? UC_CM_FRAC : 0);
	_cprman_write(ctl_off, ctl_val);
	_cprman_write(ctl_off, ctl_val | UC_CM_ENABLE);

	/*
	 * BUSY = the generator is actually running.  Skippable for
	 * clocks whose parent doesn't tick yet (CM_DSI1P sources
	 * dsi1_byte, which only starts with the PHY); mandatory for the
	 * escape clock, because a silently dead DSI1E kills LP
	 * transmission and bus turnaround while every HS path keeps
	 * working.
	 */
	if (verify_busy) {
		for (spin = 0; spin < 10000; spin++) {
			if (_cprman_read(ctl_off) & UC_CM_BUSY)
				return 0;
			bcm283x_dsi1_udelay(1);
		}
		return -1;
	}
	return 0;
}

uint32_t uc_clock_bringup_dsi1(uint32_t target_hs_hz) {
	/* BCM2711 (CM4) crystal — the only SoC these panels ship on. */
	uint32_t xosc_hz = 54000000U;
	uint32_t ndiv;
	uint32_t fdiv;
	uint32_t vco_hz;
	uint32_t phy_divider;

	_cprman_init();
	if (_cprman_read(UC_CM_LOCK) == 0)
		return 0;
	if ((_cprman_read(UC_CM_LOCK) & UC_CM_LOCK_FLOCKD) == 0)
		return 0;

	/*
	 * PLLD VCO = XOSC * (NDIV + FDIV / 2^20) / PDIV, read straight
	 * out of A2W_PLLD_CTRL/FRAC.  The feedback pre-divider is
	 * deliberately NOT consulted here — see difference A in the
	 * header comment; this is the formula the panel was proven with.
	 */
	{
		uint32_t ctl = _cprman_read(UC_A2W_PLLD_CTRL);
		uint32_t pdiv = (ctl & UC_A2W_PLL_CTRL_PDIV_MASK) >>
				UC_A2W_PLL_CTRL_PDIV_SHIFT;
		uint64_t vco;

		ndiv = ctl & UC_A2W_PLL_CTRL_NDIV_MASK;
		fdiv = _cprman_read(UC_A2W_PLLD_FRAC) & 0xfffffU;

		vco = (uint64_t)xosc_hz * ndiv;
		vco += ((uint64_t)xosc_hz * fdiv) >> 20;
		if (pdiv != 0)
			vco /= pdiv;
		vco_hz = (uint32_t)vco;
	}
	if (vco_hz == 0 || target_hs_hz == 0)
		return 0;

	/* Round to nearest integer divider, clamp to 1..255. */
	phy_divider = (vco_hz + (target_hs_hz / 2U)) / target_hs_hz;
	if (phy_divider < 1U) phy_divider = 1U;
	if (phy_divider > 255U) phy_divider = 255U;

	_plld_dsi1_set_divider(phy_divider);

	/*
	 * DSI1 escape clock: mux index 6, target 100 MHz — what
	 * vc4_dsi.c asks for via clk_set_rate(escape, 100 MHz) and what
	 * every escape-domain constant assumes (10 ns per tick).  The
	 * divider is derived from the A2W_PLLD_PER channel rate, exactly
	 * as uc_clock.c does; see difference B in the header comment for
	 * why the hs_clock/4 reading is not used.
	 */
	{
		uint32_t per_div = _cprman_read(UC_A2W_PLLD_PER) &
				UC_A2W_PLL_DIV_MASK;
		uint32_t plld_per_hz;
		uint32_t esc_div_code;

		if (per_div == 0)
			per_div = 1;
		plld_per_hz = vco_hz / per_div;

		esc_div_code = (uint32_t)((((uint64_t)plld_per_hz << 12) +
				(UC_DSI_ESC_CLOCK_HZ / 2U)) / UC_DSI_ESC_CLOCK_HZ);
		/* dsi1e wires int_bits=4, frac_bits=8: mask the unused
		 * low 4 fraction bits. */
		esc_div_code &= ~0xFU;
		if (esc_div_code < (1U << 12))
			esc_div_code = 1U << 12;
		if (esc_div_code > 0xFFF0U)
			esc_div_code = 0xFFF0U;

		if (_cm_set(UC_CM_DSI1ECTL, UC_CM_DSI1EDIV,
				UC_CM_SRC_PLLD_PER, esc_div_code,
				(esc_div_code & 0xfffU) != 0, 1) < 0) {
			/*
			 * Never went BUSY: fall back to XOSC (54 MHz,
			 * divider 1.0).  Every escape timing was computed
			 * for 100 MHz, so at 54 MHz each LP period only
			 * stretches — above every spec minimum, i.e.
			 * safe, just slower.  Better a slow escape clock
			 * than a dead one (dead = no LP TX, no bus
			 * turnaround, ever).
			 */
			if (_cm_set(UC_CM_DSI1ECTL, UC_CM_DSI1EDIV,
					UC_CM_SRC_OSC, 1U << 12, 0, 1) < 0)
				return 0;
		}
	}

	/*
	 * DSI1 pixel clock: parent must be dsi1_byte (mux index 8), the
	 * PHY's byte clock.  Any other index (in particular 0 = gnd)
	 * leaves PV1 without a clock and no pixels reach the DSI
	 * serializer.  DSI1P has int_bits = frac_bits = 0 so CM_DSI1PDIV
	 * is a no-op.
	 */
	if (_cm_set(UC_CM_DSI1PCTL, UC_CM_DSI1PDIV,
			UC_CM_SRC_DSI1_BYTE, 0, 0, 0) < 0)
		return 0;

	/* The HS bit clock PLLD_DSI1 actually generates. */
	return vco_hz / phy_divider;
}

/* ================= DSI1 controller + PHY ================= */

#define UC_DSI1_OFFSET          0x700000U

#define UC_DSI1_CTRL            0x00U
#define UC_DSI1_TXPKT1C         0x04U
#define UC_DSI1_TXPKT1H         0x08U
#define UC_DSI1_TXPKT_CMD_FIFO  0x1cU
#define UC_DSI1_TXPKT_PIX_FIFO  0x20U
#define UC_DSI1_DISP0_CTRL      0x28U
#define UC_DSI1_DISP1_CTRL      0x2cU
#define UC_DSI1_INT_STAT        0x30U
#define UC_DSI1_INT_EN          0x34U
#define UC_DSI1_STAT            0x38U
#define UC_DSI1_HSTX_TO_CNT     0x3cU
#define UC_DSI1_LPRX_TO_CNT     0x40U
#define UC_DSI1_TA_TO_CNT       0x44U
#define UC_DSI1_PR_TO_CNT       0x48U
#define UC_DSI1_PHYC            0x4cU
#define UC_DSI1_HS_CLT0         0x50U
#define UC_DSI1_HS_CLT1         0x54U
#define UC_DSI1_HS_CLT2         0x58U
#define UC_DSI1_HS_DLT3         0x5cU
#define UC_DSI1_HS_DLT4         0x60U
#define UC_DSI1_HS_DLT5         0x64U
#define UC_DSI1_HS_DLT6         0x68U
#define UC_DSI1_HS_DLT7         0x6cU
#define UC_DSI1_PHY_AFEC0       0x70U
#define UC_DSI1_PHY_AFEC1       0x74U
#define UC_DSI1_ID              0x8cU

/* CTRL bits. */
#define UC_DSI1_CTRL_EN                 (1U << 0)
#define DSI1_CTRL_HSDT_EOT_DISABLE      (1U << 11)
#define DSI1_CTRL_RX_LPDT_EOT_DISABLE   (1U << 13)
#define DSI1_CTRL_SOFT_RESET_CFG        (1U << 10)
#define DSI1_CTRL_CAL_BYTE              (1U << 9)
#define DSI1_CTRL_CLR_LDF               (1U << 7)
#define DSI1_CTRL_CLR_RXF               (1U << 6)
#define DSI1_CTRL_CLR_PDF               (1U << 5)
#define DSI1_CTRL_CLR_CDF               (1U << 4)
#define DSI1_CTRL_RESET_FIFOS   (DSI1_CTRL_CLR_LDF | DSI1_CTRL_CLR_RXF | \
				 DSI1_CTRL_CLR_PDF | DSI1_CTRL_CLR_CDF)

/* PHYC bits. */
#define UC_DSI1_PHYC_HS_CLK_CONTINUOUS  (1U << 18)
#define UC_DSI1_PHYC_CLANE_ENABLE       (1U << 16)
#define UC_DSI1_PHYC_DLANE3_ENABLE      (1U << 12)
#define UC_DSI1_PHYC_DLANE2_ENABLE      (1U << 8)
#define UC_DSI1_PHYC_DLANE1_ENABLE      (1U << 4)
#define UC_DSI1_PHYC_DLANE0_ENABLE      (1U << 0)
#define DSI1_PHYC_ESC_CLK_LPDT_SHIFT    20
#define DSI1_PHYC_ESC_CLK_LPDT_MASK     (0x3fU << 20)

/* PHY_AFEC0 bits. */
#define DSI1_PHY_AFEC0_RESET            (1U << 13)
#define DSI1_PHY_AFEC0_PD_DLANE1        (1U << 10)
#define DSI1_PHY_AFEC0_PD_DLANE2        (1U << 9)
#define DSI1_PHY_AFEC0_PD_DLANE3        (1U << 8)
#define DSI_PHY_AFEC0_PTATADJ_SHIFT     4
#define DSI_PHY_AFEC0_CTATADJ_SHIFT     0
#define DSI1_PHY_AFEC0_IDR_CLANE_SHIFT  17
#define DSI1_PHY_AFEC0_IDR_DLANE0_SHIFT 20
#define DSI1_PHY_AFEC0_IDR_DLANE1_SHIFT 23
#define DSI1_PHY_AFEC0_IDR_DLANE2_SHIFT 26
#define DSI1_PHY_AFEC0_IDR_DLANE3_SHIFT 29
#define UC_DSI1_PHY_AFEC0_LATCH_ULPS    (1U << 14)

/* INT / STAT bits. */
#define UC_DSI1_INT_TXPKT1_DONE         (1U << 1)
#define UC_DSI1_INT_ALWAYS_ENABLED      ((1U << 6) | (1U << 7) | (1U << 8) | \
					 (1U << 9) | (1U << 10) | (1U << 11) | \
					 (1U << 12) | (1U << 13))
#define UC_DSI1_STAT_TXPKT1_DONE        (1U << 1)
#define UC_DSI1_STAT_PHY_D0_STOP        (1U << 24)
#define UC_DSI1_STAT_PHY_D1_STOP        (1U << 26)
#define UC_DSI1_STAT_PHY_D2_STOP        (1U << 28)
#define UC_DSI1_STAT_PHY_D3_STOP        (1U << 30)

/* DISP0 / DISP1 CTRL. */
#define DISP1_PFORMAT_32BIT_LE_SHIFT    1
#define DISP1_PFORMAT_32BIT_LE_VAL      2
#define DISP1_ENABLE                    (1U << 0)
#define DISP0_ENABLE                    (1U << 0)

/* TXPKT1C / TXPKT1H fields. */
#define TXPKT1C_CMD_EN                  (1U << 0)
#define TXPKT1C_CMD_TYPE_LONG           (1U << 2)
#define TXPKT1C_CMD_CTRL_TX             (0U << 4)
#define TXPKT1C_CMD_REPEAT_SHIFT        10
#define TXPKT1C_DISPLAY_NO_SHORT        (0U << 8)
#define TXPKT1C_DISPLAY_NO_SECONDARY    (2U << 8)
#define TXPKT1H_BC_DT_SHIFT             0
#define TXPKT1H_BC_PARAM_SHIFT          8
#define TXPKT1H_BC_CMDFIFO_SHIFT        24

/* HS_CLT0..HS_DLT7 field shifts. */
#define HS_CLT0_CZERO_SHIFT             18
#define HS_CLT0_CPRE_SHIFT              9
#define HS_CLT0_CPREP_SHIFT             0
#define HS_CLT1_CTRAIL_SHIFT            9
#define HS_CLT1_CPOST_SHIFT             0
#define HS_CLT2_WUP_SHIFT               0
#define HS_DLT3_EXIT_SHIFT              18
#define HS_DLT3_ZERO_SHIFT              9
#define HS_DLT3_PRE_SHIFT               0
#define HS_DLT4_ANLAT_SHIFT             18
#define HS_DLT4_TRAIL_SHIFT             9
#define HS_DLT4_LPX_SHIFT               0
#define HS_DLT5_INIT_SHIFT              0
#define HS_DLT6_TA_GET_SHIFT            24
#define HS_DLT6_TA_SURE_SHIFT           16
#define HS_DLT6_TA_GO_SHIFT             8
#define HS_DLT6_LP_LPX_SHIFT            0
#define HS_DLT7_LP_WUP_SHIFT            0

/* Both ClockworkPi panels are 4-lane RGB888 (24bpp / 4 lanes = 6). */
#define UC_DSI_LANES            4U
#define UC_DSI_FORMAT_RGB888    3U
#define UC_DSI_PIXEL_DIVIDER    6U

static volatile uint32_t* _dsi1 = 0;

/* HS bit clock used for the PHY timing computation. */
static uint32_t _hs_clock_hz = 0;

static void _dsi1_init(void) {
	if (_dsi1 == 0 && _mmio_base != 0)
		_dsi1 = (volatile uint32_t*)(uintptr_t)(_mmio_base +
				UC_DSI1_OFFSET);
}

static uint32_t _dsi1_read(uint32_t off) {
	_dsi1_init();
	if (_dsi1 == 0)
		return 0;
	return _dsi1[off / 4];
}

static void _dsi1_write(uint32_t off, uint32_t val) {
	_dsi1_init();
	if (_dsi1 == 0)
		return;
	_dsi1[off / 4] = val;
}

/*
 * Unit-interval-in-ns for the target HS bit clock.  The PHY clock is
 * DDR, so 1 UI = 2 clock periods, hence 500e6/hz.
 */
static uint32_t _ui_ns(void) {
	return (500000000U + _hs_clock_hz - 1U) / _hs_clock_hz;
}

/* vc4_dsi.c::dsi_hs_timing().  Round up to a multiple of 8 (byte clock). */
static uint32_t _hst(uint32_t ui_ns, uint32_t ns, uint32_t ui) {
	uint32_t v = ui + ((ns + ui_ns - 1U) / ui_ns);
	return (v + 7U) & ~7U;
}

/* ESC clock assumed 100 MHz => 10 ns per tick. */
static uint32_t _est(uint32_t ns) {
	return (ns + 9U) / 10U;
}

static uint32_t _hst_max(uint32_t a, uint32_t b) {
	return a > b ? a : b;
}

int uc_dsi_alive(void) {
	_dsi1_init();
	if (_dsi1 == 0)
		return -1;
	return (_dsi1_read(UC_DSI1_ID) == 0x00647369U) ? 0 : -1;
}

/*
 * After bring-up all four data lanes must sit in LP-11 STOP: that is
 * the analog PHY actually driving the lines.
 */
int uc_dsi_lanes_stopped(void) {
	uint32_t stat_stop;

	_dsi1_init();
	if (_dsi1 == 0)
		return -1;
	stat_stop = UC_DSI1_STAT_PHY_D0_STOP | UC_DSI1_STAT_PHY_D1_STOP |
			UC_DSI1_STAT_PHY_D2_STOP | UC_DSI1_STAT_PHY_D3_STOP;
	return ((_dsi1_read(UC_DSI1_STAT) & stat_stop) == stat_stop) ? 0 : -1;
}

int uc_dsi_bringup(uint32_t hs_clock_hz) {
	uint32_t ui_ns;
	uint32_t lpx;
	uint32_t afec0;

	_dsi1_init();
	if (_dsi1 == 0 || hs_clock_hz == 0)
		return -1;
	_hs_clock_hz = hs_clock_hz;

	ui_ns = _ui_ns();
	lpx = _est(60);   /* Minimum LP state = 60ns => 6 escape ticks. */

	/* Reset the controller + all FIFOs.  EN stays CLEAR here and all
	 * the way through the timing bank — see difference C. */
	_dsi1_write(UC_DSI1_CTRL,
			DSI1_CTRL_SOFT_RESET_CFG | DSI1_CTRL_RESET_FIFOS);
	_dsi1_write(UC_DSI1_CTRL,
			DSI1_CTRL_HSDT_EOT_DISABLE | DSI1_CTRL_RX_LPDT_EOT_DISABLE);

	/* Clear all STAT bits (W1C). */
	_dsi1_write(UC_DSI1_STAT, _dsi1_read(UC_DSI1_STAT));

	/*
	 * Keep the error/timeout interrupts enabled from the start and
	 * flush any latched interrupt state.  Some INT_STAT reporting is
	 * gated by INT_EN, so leaving INT_EN at 0 hides transfer
	 * completion from the DCS polling loop.
	 */
	_dsi1_write(UC_DSI1_INT_EN, UC_DSI1_INT_ALWAYS_ENABLED);
	_dsi1_write(UC_DSI1_INT_STAT, _dsi1_read(UC_DSI1_INT_STAT));

	/*
	 * Bring the analog PHY out of powerdown, but keep AFEC0 RESET
	 * set until the clocks have been ticking for a bit.  IDR values
	 * of 6 for all lanes match vc4's DSI1 path.
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
	_dsi1_write(UC_DSI1_PHY_AFEC0, afec0);
	_dsi1_write(UC_DSI1_PHY_AFEC1, 0);
	bcm283x_dsi1_mdelay(1);

	/* HS timing regs — verbatim from vc4_dsi_encoder_enable(). */
	_dsi1_write(UC_DSI1_HS_CLT0,
			(_hst(ui_ns, 262, 0) << HS_CLT0_CZERO_SHIFT) |
			(_hst(ui_ns, 0,   8) << HS_CLT0_CPRE_SHIFT)  |
			(_hst(ui_ns, 38,  0) << HS_CLT0_CPREP_SHIFT));
	_dsi1_write(UC_DSI1_HS_CLT1,
			(_hst(ui_ns, 60, 0) << HS_CLT1_CTRAIL_SHIFT) |
			(_hst(ui_ns, 60, 52) << HS_CLT1_CPOST_SHIFT));
	_dsi1_write(UC_DSI1_HS_CLT2,
			(_hst(ui_ns, 1000000, 0) << HS_CLT2_WUP_SHIFT));
	_dsi1_write(UC_DSI1_HS_DLT3,
			(_hst(ui_ns, 100, 0) << HS_DLT3_EXIT_SHIFT) |
			(_hst(ui_ns, 105, 6) << HS_DLT3_ZERO_SHIFT) |
			(_hst(ui_ns, 40,  4) << HS_DLT3_PRE_SHIFT));
	_dsi1_write(UC_DSI1_HS_DLT4,
			(_hst(ui_ns, lpx * 10, 0) << HS_DLT4_LPX_SHIFT) |
			(_hst_max(_hst(ui_ns, 0, 8),
				  _hst(ui_ns, 60, 4)) << HS_DLT4_TRAIL_SHIFT) |
			(0U << HS_DLT4_ANLAT_SHIFT));
	_dsi1_write(UC_DSI1_HS_DLT5,
			(_hst(ui_ns, 5 * 1000 * 1000, 0) << HS_DLT5_INIT_SHIFT));
	_dsi1_write(UC_DSI1_HS_DLT6,
			((lpx * 5) << HS_DLT6_TA_GET_SHIFT) |
			(lpx       << HS_DLT6_TA_SURE_SHIFT) |
			((lpx * 4) << HS_DLT6_TA_GO_SHIFT) |
			(lpx       << HS_DLT6_LP_LPX_SHIFT));
	_dsi1_write(UC_DSI1_HS_DLT7,
			(_est(1000000) << HS_DLT7_LP_WUP_SHIFT));

	/* PHYC: clock lane + all four data lanes + continuous HS clock. */
	_dsi1_write(UC_DSI1_PHYC,
			UC_DSI1_PHYC_DLANE0_ENABLE |
			UC_DSI1_PHYC_DLANE1_ENABLE |
			UC_DSI1_PHYC_DLANE2_ENABLE |
			UC_DSI1_PHYC_DLANE3_ENABLE |
			UC_DSI1_PHYC_CLANE_ENABLE |
			UC_DSI1_PHYC_HS_CLK_CONTINUOUS |
			(((lpx - 1U) << DSI1_PHYC_ESC_CLK_LPDT_SHIFT) &
			 DSI1_PHYC_ESC_CLK_LPDT_MASK));

	/* Byte calibration. */
	_dsi1_write(UC_DSI1_CTRL, _dsi1_read(UC_DSI1_CTRL) | DSI1_CTRL_CAL_BYTE);

	/* Timeouts (vc4 uses disable / large fixed values). */
	_dsi1_write(UC_DSI1_HSTX_TO_CNT, 0);
	_dsi1_write(UC_DSI1_LPRX_TO_CNT, 0xffffffU);
	_dsi1_write(UC_DSI1_TA_TO_CNT,   100000);
	_dsi1_write(UC_DSI1_PR_TO_CNT,   100000);

	/* DISP1 for long command payloads through the pixel FIFO. */
	_dsi1_write(UC_DSI1_DISP1_CTRL,
			(DISP1_PFORMAT_32BIT_LE_VAL << DISP1_PFORMAT_32BIT_LE_SHIFT) |
			DISP1_ENABLE);

	/* Ungate the block — only now, with the whole timing bank in. */
	_dsi1_write(UC_DSI1_CTRL, _dsi1_read(UC_DSI1_CTRL) | UC_DSI1_CTRL_EN);

	/* Release AFEC RESET. */
	_dsi1_write(UC_DSI1_PHY_AFEC0,
			_dsi1_read(UC_DSI1_PHY_AFEC0) & ~DSI1_PHY_AFEC0_RESET);

	/*
	 * vc4_dsi_ulps(false) at cold boot is a no-op: upstream checks
	 * the AFEC0 LATCH_ULPS latch and returns early because it is
	 * already clear.  Both ClockworkPi panels want a continuous HS
	 * clock, so the clock lane is never put into ULPS either.
	 */
	if (_dsi1_read(UC_DSI1_PHY_AFEC0) & UC_DSI1_PHY_AFEC0_LATCH_ULPS) {
		uint32_t stat_stop = UC_DSI1_STAT_PHY_D0_STOP |
				UC_DSI1_STAT_PHY_D1_STOP |
				UC_DSI1_STAT_PHY_D2_STOP |
				UC_DSI1_STAT_PHY_D3_STOP;
		uint32_t phyc_ulps = (1U << 1) | (1U << 5) | (1U << 9) | (1U << 13);
		int spin;

		_dsi1_write(UC_DSI1_PHYC, _dsi1_read(UC_DSI1_PHYC) & ~phyc_ulps);
		for (spin = 0; spin < 200000; spin++) {
			if ((_dsi1_read(UC_DSI1_STAT) & stat_stop) == stat_stop)
				break;
			bcm283x_dsi1_udelay(1);
		}
	}

	/*
	 * DISP0_CTRL stays 0 all the way through the panel's DCS init;
	 * uc_dsi_video_mode() writes the complete video-mode value,
	 * ENABLE bit included, in one shot afterwards.
	 */
	return 0;
}

static int _mipi_is_long(uint8_t dt) {
	/* Short packets have the top two bits 00b/01b, long 10b/11b. */
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

	_dsi1_init();
	if (_dsi1 == 0)
		return -1;

	if (is_long) {
		uint32_t pix_fifo_len = 0;

		/*
		 * Payloads up to 16 bytes fit the byte-oriented command
		 * FIFO alone.  Longer ones keep the len%4 residue — from
		 * the START of the payload — in the command FIFO and
		 * stream the rest through the pixel FIFO as
		 * little-endian 32-bit words, routed to the "secondary
		 * display" datapath.  cwd686's 39-byte GAMMA entries take
		 * this path.
		 */
		if (len <= 16) {
			cmd_fifo_len = len;
		} else {
			cmd_fifo_len = len % 4U;
			pix_fifo_len = (len - cmd_fifo_len) / 4U;
		}
		pkth = ((uint32_t)data_type << TXPKT1H_BC_DT_SHIFT) |
		       (((uint32_t)len & 0xffffU) << TXPKT1H_BC_PARAM_SHIFT) |
		       ((cmd_fifo_len & 0xffU) << TXPKT1H_BC_CMDFIFO_SHIFT);
		pktc |= TXPKT1C_CMD_TYPE_LONG;
		if (pix_fifo_len != 0)
			pktc |= TXPKT1C_DISPLAY_NO_SECONDARY;
		for (i = 0; i < cmd_fifo_len; i++)
			_dsi1_write(UC_DSI1_TXPKT_CMD_FIFO, payload[i]);
		for (i = 0; i < pix_fifo_len; i++) {
			const uint8_t* pix = payload + cmd_fifo_len + i * 4U;
			_dsi1_write(UC_DSI1_TXPKT_PIX_FIFO,
					(uint32_t)pix[0] |
					((uint32_t)pix[1] << 8) |
					((uint32_t)pix[2] << 16) |
					((uint32_t)pix[3] << 24));
		}
	} else {
		uint32_t p0 = (len > 0 && payload) ? payload[0] : 0;
		uint32_t p1 = (len > 1 && payload) ? payload[1] : 0;
		pkth = ((uint32_t)data_type << TXPKT1H_BC_DT_SHIFT) |
		       ((p0 | (p1 << 8)) << TXPKT1H_BC_PARAM_SHIFT);
	}

	/*
	 * HS command mode, NOT LP: neither panel-*.c sets
	 * MIPI_DSI_MODE_LPM, so upstream never passes
	 * MIPI_DSI_MSG_USE_LPM and TXPKT1C_CMD_MODE_LP stays clear.
	 * Forcing LP puts a completely different waveform on the wire
	 * and the panel latches nothing (black panel, backlight on).
	 */
	pktc |= TXPKT1C_CMD_CTRL_TX;
	pktc |= (1U << TXPKT1C_CMD_REPEAT_SHIFT);
	pktc |= TXPKT1C_CMD_EN;
	pktc |= TXPKT1C_DISPLAY_NO_SHORT;

	/*
	 * TXPKT1_DONE must be enabled in INT_EN before every transfer,
	 * on top of the always-enabled error set: INT_STAT reporting can
	 * be gated by INT_EN on this block, so without this write a
	 * successfully transmitted packet may never show DONE.
	 */
	_dsi1_write(UC_DSI1_INT_EN,
			UC_DSI1_INT_ALWAYS_ENABLED | UC_DSI1_INT_TXPKT1_DONE);
	/* Clear stale completion state in both status registers. */
	_dsi1_write(UC_DSI1_INT_STAT, UC_DSI1_INT_TXPKT1_DONE);
	_dsi1_write(UC_DSI1_STAT, UC_DSI1_STAT_TXPKT1_DONE);

	_dsi1_write(UC_DSI1_TXPKT1H, pkth);
	_dsi1_write(UC_DSI1_TXPKT1C, pktc);

	/* Poll ~200 ms; accept completion from either INT_STAT or the
	 * raw (non-gated) STAT copy of TXPKT1_DONE. */
	for (spin = 0; spin < 200000; spin++) {
		if ((_dsi1_read(UC_DSI1_INT_STAT) & UC_DSI1_INT_TXPKT1_DONE) ||
		    (_dsi1_read(UC_DSI1_STAT) & UC_DSI1_STAT_TXPKT1_DONE)) {
			_dsi1_write(UC_DSI1_INT_STAT, UC_DSI1_INT_TXPKT1_DONE);
			_dsi1_write(UC_DSI1_STAT, UC_DSI1_STAT_TXPKT1_DONE);
			_dsi1_write(UC_DSI1_INT_EN, UC_DSI1_INT_ALWAYS_ENABLED);
			return 0;
		}
		bcm283x_dsi1_udelay(1);
	}

	/* Reset the transmit FIFO the same way vc4 does on error. */
	_dsi1_write(UC_DSI1_TXPKT1C,
			_dsi1_read(UC_DSI1_TXPKT1C) & ~TXPKT1C_CMD_EN);
	bcm283x_dsi1_udelay(1);
	_dsi1_write(UC_DSI1_CTRL,
			_dsi1_read(UC_DSI1_CTRL) | DSI1_CTRL_RESET_FIFOS);
	_dsi1_write(UC_DSI1_TXPKT1C, 0);
	_dsi1_write(UC_DSI1_INT_EN, UC_DSI1_INT_ALWAYS_ENABLED);
	return -1;
}

void uc_dsi_video_mode(void) {
	uint32_t v;

	_dsi1_init();
	if (_dsi1 == 0)
		return;

	/*
	 * vc4_dsi_encoder_enable() for MIPI_DSI_MODE_VIDEO, verbatim:
	 * the whole video-mode DISP0_CTRL — PIX_CLK_DIV, PFORMAT,
	 * LP_STOP_PERFRAME, ST_END and ENABLE — is one single write,
	 * done only after the panel DCS init.  DISP0_CTRL was 0 until
	 * now.
	 */
	v = ((uint32_t)UC_DSI_PIXEL_DIVIDER << 13) |
	    ((uint32_t)UC_DSI_FORMAT_RGB888 << 2) |
	    (2U << 11) |          /* LP_STOP_CTRL = LP_STOP_PERFRAME */
	    (1U << 4) |           /* ST_END */
	    DISP0_ENABLE;
	_dsi1_write(UC_DSI1_DISP0_CTRL, v);
}
