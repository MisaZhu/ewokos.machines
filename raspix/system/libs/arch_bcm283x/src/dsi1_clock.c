#include "dsi1_internal.h"

#include <stdint.h>

/*
 * CPRMAN (clock/PLL) register layout — relative to MMIO+0x101000,
 * identical on BCM2835/2837 (gen4) and BCM2711 (gen5).  Only the
 * XOSC frequency differs between the SoCs (19.2 MHz vs 54 MHz).
 */

/* CM_*CTL common bits (clk-bcm2835.c). */
#define CM_ENABLE            (1U << 4)
#define CM_KILL              (1U << 5)
#define CM_BUSY              (1U << 7)
#define CM_FRAC              (1U << 9)
#define CM_SRC_MASK          0xfU
#define CM_SRC_OSC           1U
/*
 * DSI0/DSI1 CM parent mux (bcm2835_clock_dsi0/1_parents in
 * clk-bcm2835.c), identical layout on both ports:
 *   0=gnd 1=xosc 2/3=testdebug 4=dsiX_ddr 6=dsiX_ddr2 8=dsiX_byte.
 * The dsiX_* taps come from the DSI HS clock: ddr2 = bit clock / 4
 * (escape source), byte = bit clock / 8 (pixel source).
 */
#define CM_SRC_DSI_DDR2      6U
#define CM_SRC_DSI_BYTE      8U

/* Selected CPRMAN CM_* offsets. */
#define CM_OSCCOUNT          0x100U
#define CM_PLLA              0x104U
#define CM_PLLD              0x10cU
#define CM_LOCK              0x114U
/*
 * Per-port DSI escape/pixel clock generator offsets (clk-bcm2835.c).
 * DSI0 lives low in the CM map (0x58..0x64), DSI1 high (0x158..0x164);
 * both were proven against real hardware (DSI1 on the uConsole CM4).
 */
#define CM_DSI0ECTL          0x058U
#define CM_DSI0EDIV          0x05cU
#define CM_DSI0PCTL          0x060U
#define CM_DSI0PDIV          0x064U
#define CM_DSI1ECTL          0x158U   /* DSI1 escape-clock control */
#define CM_DSI1EDIV          0x15cU
#define CM_DSI1PCTL          0x160U   /* DSI1 pixel-clock control  */
#define CM_DSI1PDIV          0x164U
/*
 * DSI0 HS-clock source selector: 0 = PLLA_DSI0 (the firmware default
 * on Pi3), 1 = PLLD_DSI0.  We program PLLD_DSI0 ourselves, so the DSI0
 * path must flip this to 1.  DSI1's HS clock always comes from
 * PLLD_DSI1; there is no equivalent selector.
 */
#define CM_DSI0HSCK          0x120U
#define CM_DSI0HSCK_SELPLLD  (1U << 0)

/* TCNT hardware frequency counter (bcm2835_measure_tcnt_mux). */
#define CM_TCNTCTL          0x0c0U
#define CM_TCNTCNT           0x0c4U
#define CM_TCNT_SRC1_SHIFT   12U

/* CM_PLLD load/hold flags, per DSI channel. */
#define CM_PLLD_HOLDDSI0     (1U << 1)
#define CM_PLLD_LOADDSI0     (1U << 0)
#define CM_PLLD_HOLDDSI1     (1U << 3)
#define CM_PLLD_LOADDSI1     (1U << 2)

/* CM_PLLA has the same DSI0 load/hold layout. */
#define CM_PLLA_HOLDDSI0     (1U << 1)
#define CM_PLLA_LOADDSI0     (1U << 0)

/* Analog-block reset bit, common to all CM_PLLx registers. */
#define CM_PLL_ANARST        (1U << 8)

/* CM_LOCK FLOCK bits. */
#define CM_LOCK_FLOCKA       (1U << 8)
#define CM_LOCK_FLOCKD       (1U << 11)

/* A2W PLLD wrappers. */
#define A2W_PLLD_CTRL        0x1140U
#define A2W_PLLD_FRAC        0x1240U
#define A2W_PLLD_PER         0x1540U
#define A2W_PLLD_DSI0        0x1340U
#define A2W_PLLD_DSI1        0x1640U
#define A2W_PLLD_ANA1        0x1054U  /* ana_reg_base(0x1050) + 4 */

/* A2W PLLA wrappers — the spare PLL Linux dedicates to DSI0. */
#define A2W_PLLA_CTRL        0x1100U
#define A2W_PLLA_FRAC        0x1200U
#define A2W_PLLA_DSI0        0x1300U
#define A2W_PLLA_ANA1        0x1014U  /* ana_reg_base(0x1010) + 4 */

/*
 * ANA1 feedback pre-divider (bcm2835_ana_default.fb_prediv_mask).
 * When set, the PLL feedback path is divided by 2, i.e. the VCO runs
 * at TWICE the rate the NDIV/FDIV registers alone suggest.  The Pi3
 * firmware boots PLLD this way: CTRL reads NDIV=52 ("1 GHz") but the
 * real VCO is 2 GHz — cross-checked by PLLD_PER div 4 = the well-known
 * 500 MHz plld_per.  Linux bcm2835_pll_get_rate() doubles both NDIV
 * and FDIV when this bit is set; miss it and every divider derived
 * from the VCO is 2x off (a "1 Gbps" DSI link actually at 2 Gbps).
 */
#define A2W_PLL_ANA1_FB_PREDIV       (1U << 14)

/* A2W_PLL_CTRL bits. */
#define A2W_PLL_CTRL_PDIV_SHIFT      12
#define A2W_PLL_CTRL_PDIV_MASK       0x00007000U
#define A2W_PLL_CTRL_NDIV_MASK       0x000003ffU
#define A2W_PLL_CTRL_PWRDN           (1U << 16)
#define A2W_PLL_CTRL_PRST_DISABLE    (1U << 17)

/* A2W_PLLx_FRAC: 20 fractional bits. */
#define A2W_PLL_FRAC_BITS            20
#define A2W_PLL_FRAC_MASK            0xfffffU

/* A2W PLL channel divider register bits. */
#define A2W_PLL_CHANNEL_DISABLE      (1U << 8)
#define A2W_PLL_DIV_MASK             0xffU

/* Set when DSI1E had to fall back from PLLD_PER to XOSC. */
static int _dsi1e_fallback = 0;

/*
 * Program an A2W PLLx_DSIy integer channel divider and enable the
 * channel.  After the divider is set we pulse the CM_PLLx LOADDSIy
 * flag to latch it.
 */
static void _pll_dsi_set_divider(uint32_t cm_pll, uint32_t a2w_off,
		uint32_t hold_bit, uint32_t load_bit, uint32_t divider) {
	uint32_t ch;

	/* Hold the DSI phase during the update. */
	dsi1_cprman_write(cm_pll,
			dsi1_cprman_read(cm_pll) | hold_bit);

	ch = dsi1_cprman_read(a2w_off);
	ch &= ~A2W_PLL_DIV_MASK;
	ch |= (divider & A2W_PLL_DIV_MASK);
	ch &= ~A2W_PLL_CHANNEL_DISABLE;
	dsi1_cprman_write(a2w_off, ch);

	/* Pulse LOADDSIx to latch the new divider. */
	dsi1_cprman_write(cm_pll,
			dsi1_cprman_read(cm_pll) | load_bit);
	bcm283x_dsi1_udelay(1);
	dsi1_cprman_write(cm_pll,
			dsi1_cprman_read(cm_pll) & ~load_bit);

	/* Release the hold. */
	dsi1_cprman_write(cm_pll,
			dsi1_cprman_read(cm_pll) & ~hold_bit);
}

/*
 * CM_*CTL update: KILL the clock, wait for BUSY to clear, rewrite the
 * source + divider, then set ENABLE — the same pattern clk-bcm2835.c
 * uses when reparenting a clock.
 *
 * `div_code` is the raw CM_*DIV register value: ALWAYS 12.12 fixed
 * point (integer [23:12], fraction [11:0]); a clock's int_bits/
 * frac_bits only say which of those bits are wired up.  `use_frac`
 * mirrors bcm2835_clock_set_rate: CM_FRAC must be set in CTL whenever
 * any fraction bit is non-zero.
 */
static int _cm_set(uint32_t ctl_off, uint32_t div_off,
		uint32_t src, uint32_t div_code, int use_frac, int verify_busy) {
	uint32_t ctl;
	uint32_t ctl_val;
	int spin;

	ctl = dsi1_cprman_read(ctl_off);
	if (ctl & CM_ENABLE) {
		dsi1_cprman_write(ctl_off, (ctl & ~CM_ENABLE) | CM_KILL);
		for (spin = 0; spin < 10000; spin++) {
			if ((dsi1_cprman_read(ctl_off) & CM_BUSY) == 0) {
				break;
			}
			bcm283x_dsi1_udelay(1);
		}
		if ((dsi1_cprman_read(ctl_off) & CM_BUSY) != 0) {
			return -1;
		}
	}

	/* CM_*DIV: raw register value, integer part pre-shifted. */
	dsi1_cprman_write(div_off, div_code & 0xffffffU);
	/* Set SRC (bits [3:0]) + FRAC, then set ENABLE. */
	ctl_val = (src & CM_SRC_MASK) | (use_frac ? CM_FRAC : 0);
	dsi1_cprman_write(ctl_off, ctl_val);
	dsi1_cprman_write(ctl_off, ctl_val | CM_ENABLE);

	/*
	 * BUSY = the generator is actually running.  Skippable for
	 * clocks whose parent doesn't tick yet at programming time
	 * (CM_DSI1P sources dsi1_byte, which only starts with the
	 * PHY); mandatory for the escape clock, because a silently
	 * dead DSI1E kills LP transmission and bus turnaround while
	 * every HS path keeps working.
	 */
	if (verify_busy) {
		for (spin = 0; spin < 10000; spin++) {
			if (dsi1_cprman_read(ctl_off) & CM_BUSY) {
				return 0;
			}
			bcm283x_dsi1_udelay(1);
		}
		return -1;
	}
	return 0;
}

/*
 * Read PLLD's actual VCO frequency:
 *   VCO = XOSC * (NDIV + FDIV / 2^20) * (FB prediv ? 2 : 1) / PDIV
 * NDIV lives in A2W_PLLD_CTRL[9:0], FDIV in A2W_PLLD_FRAC[19:0], the
 * FB pre-divider in ANA1[14].  Getting any factor wrong means every
 * divider derived from the VCO is off and the panel never locks on
 * the HS clock: the Pi3 firmware boots PLLD with the FB prediv SET
 * (registers say 1 GHz, real VCO 2 GHz), which put the "1 Gbps" HS
 * link at an unreceivable 2 Gbps while every on-chip block (HVS, PV,
 * DSI host — all fed from divided-down taps) kept running happily.
 */
static uint32_t _plld_vco_hz(void) {
	uint32_t ctl = dsi1_cprman_read(A2W_PLLD_CTRL);
	uint32_t pdiv = (ctl & A2W_PLL_CTRL_PDIV_MASK) >>
			A2W_PLL_CTRL_PDIV_SHIFT;
	uint32_t ndiv = ctl & A2W_PLL_CTRL_NDIV_MASK;
	uint32_t fdiv = dsi1_cprman_read(A2W_PLLD_FRAC) & 0xfffffU;
	uint32_t xosc_hz = dsi1_xosc_hz();
	uint64_t vco;

	if (dsi1_cprman_read(A2W_PLLD_ANA1) & A2W_PLL_ANA1_FB_PREDIV) {
		ndiv *= 2;
		fdiv *= 2;
	}

	vco = (uint64_t)xosc_hz * ndiv;
	vco += ((uint64_t)xosc_hz * fdiv) >> 20;
	if (pdiv != 0) {
		vco /= pdiv;
	}
	return (uint32_t)vco;
}

/*
 * Retune the PLLA VCO to `vco_hz` and (re)lock it — the bcm2835_pll
 * set_rate/on sequence from clk-bcm2835.c.  PLLA is the spare PLL on
 * these SoCs: the firmware locks it at boot but leaves every channel
 * disabled, which is exactly why Linux hangs DSI0 off it — unlike
 * PLLD (whose VCO feeds core/per consumers and must not move), PLLA
 * can be set to make the DSI0 HS clock EXACTLY the panel's nominal
 * rate.  Caller must keep vco_hz in the 600–1600 MHz band so the
 * firmware's analog (ANA/FB-prediv) setup stays valid untouched.
 * Returns the achieved VCO rate, 0 on lock timeout.
 */
static uint32_t _plla_set_vco(uint32_t vco_hz) {
	uint32_t xosc_hz = dsi1_xosc_hz();
	uint64_t div;
	uint32_t ndiv, fdiv, ctrl;
	uint32_t prediv;
	int spin;

	if (xosc_hz == 0) {
		return 0;
	}
	/* Honour the firmware's FB prediv: with it set the VCO runs at
	 * 2x the programmed multiplier, so halve what we write. */
	prediv = (dsi1_cprman_read(A2W_PLLA_ANA1) & A2W_PLL_ANA1_FB_PREDIV)
			? 2U : 1U;
	div = (((uint64_t)vco_hz << A2W_PLL_FRAC_BITS) + xosc_hz / 2U) /
			xosc_hz / prediv;
	ndiv = (uint32_t)(div >> A2W_PLL_FRAC_BITS);
	fdiv = (uint32_t)(div & A2W_PLL_FRAC_MASK);

	/* Power the PLL up and take the analog block out of reset —
	 * both no-ops when the firmware already runs PLLA. */
	ctrl = dsi1_cprman_read(A2W_PLLA_CTRL);
	ctrl &= ~A2W_PLL_CTRL_PWRDN;
	dsi1_cprman_write(A2W_PLLA_CTRL, ctrl);
	dsi1_cprman_write(CM_PLLA,
			dsi1_cprman_read(CM_PLLA) & ~CM_PLL_ANARST);

	/* New multiplier: FRAC first, then NDIV + PDIV=1. */
	dsi1_cprman_write(A2W_PLLA_FRAC, fdiv);
	ctrl &= ~(A2W_PLL_CTRL_PDIV_MASK | A2W_PLL_CTRL_NDIV_MASK);
	ctrl |= (1U << A2W_PLL_CTRL_PDIV_SHIFT) |
			(ndiv & A2W_PLL_CTRL_NDIV_MASK);
	dsi1_cprman_write(A2W_PLLA_CTRL, ctrl);

	for (spin = 0; spin < 10000; spin++) {
		if (dsi1_cprman_read(CM_LOCK) & CM_LOCK_FLOCKA) {
			break;
		}
		bcm283x_dsi1_udelay(1);
	}
	if ((dsi1_cprman_read(CM_LOCK) & CM_LOCK_FLOCKA) == 0) {
		return 0;
	}

	/* Release the post-divider reset (no-op if already out). */
	dsi1_cprman_write(A2W_PLLA_CTRL,
			dsi1_cprman_read(A2W_PLLA_CTRL) |
			A2W_PLL_CTRL_PRST_DISABLE);

	return (uint32_t)(((uint64_t)xosc_hz * ndiv +
			(((uint64_t)xosc_hz * fdiv) >> A2W_PLL_FRAC_BITS)) *
			prediv);
}

/*
 * vc4_dsi_bridge_mode_fixup(): PLLD_DSI1 only has an integer divider,
 * so the HS bit clock lands on vco/round-up divider and the pixel clock
 * is pll/divider.  The extra clock is paid back by extending htotal
 * (all of it into hfp) so the panel refresh stays at its nominal value.
 */
static int _mode_fixup(const bcm283x_dsi1_mode_t* mode, uint32_t vco_hz,
		bcm283x_dsi1_adjusted_mode_t* adj) {
	uint32_t divider;
	uint32_t pix_clk_div;
	uint64_t pll_clock;
	uint64_t pixel_clock;
	uint32_t htotal;
	uint32_t adj_htotal;

	if (mode->lanes == 0 || mode->pixel_clock_hz == 0 || vco_hz == 0) {
		return -1;
	}

	/* DISP0 PIX_CLK_DIV = bpp / lanes, 24bpp (RGB888) panels. */
	pix_clk_div = 24U / mode->lanes;
	if (pix_clk_div == 0) {
		return -1;
	}

	pll_clock = (uint64_t)mode->pixel_clock_hz * pix_clk_div;

	/* Find what divider gets us a faster clock than requested,
	 * then calculate back to its actual rate (vc4 mode_fixup). */
	for (divider = 1; divider < 255; divider++) {
		if (vco_hz / (divider + 1) < pll_clock) {
			break;
		}
	}
	pll_clock = vco_hz / divider;
	pixel_clock = pll_clock / pix_clk_div;
	if (pixel_clock == 0) {
		return -1;
	}

	/* Adjust HFP to keep vrefresh the same. */
	htotal = mode->width + mode->hfp + mode->hsw + mode->hbp;
	adj_htotal = (uint32_t)((pixel_clock * htotal) / mode->pixel_clock_hz);

	adj->width = mode->width;
	adj->height = mode->height;
	adj->hsw = mode->hsw;
	adj->hbp = mode->hbp;
	adj->hfp = adj_htotal - mode->width - mode->hsw - mode->hbp;
	adj->vfp = mode->vfp;
	adj->vsw = mode->vsw;
	adj->vbp = mode->vbp;
	adj->pixel_clock_hz = (uint32_t)pixel_clock;
	adj->hs_clock_hz = (uint32_t)pll_clock;
	adj->pix_clk_divider = pix_clk_div;
	return 0;
}

int bcm283x_dsi1_clock_bringup(const bcm283x_dsi1_mode_t* mode,
		bcm283x_dsi1_adjusted_mode_t* adj) {
	uint32_t vco_hz;
	uint32_t phy_divider;
	uint32_t esc_div_code;
	uint32_t esc_parent_hz;
	uint32_t cm_ectl, cm_ediv, cm_pctl, cm_pdiv;
	uint32_t a2w_dsi, hold_bit, load_bit;
	int port = dsi1_port();

	if (mode == 0 || adj == 0) {
		return -1;
	}

	if (port == 0) {
		cm_ectl = CM_DSI0ECTL;
		cm_ediv = CM_DSI0EDIV;
		cm_pctl = CM_DSI0PCTL;
		cm_pdiv = CM_DSI0PDIV;
		a2w_dsi = A2W_PLLA_DSI0;
		hold_bit = CM_PLLA_HOLDDSI0;
		load_bit = CM_PLLA_LOADDSI0;
	} else {
		cm_ectl = CM_DSI1ECTL;
		cm_ediv = CM_DSI1EDIV;
		cm_pctl = CM_DSI1PCTL;
		cm_pdiv = CM_DSI1PDIV;
		a2w_dsi = A2W_PLLD_DSI1;
		hold_bit = CM_PLLD_HOLDDSI1;
		load_bit = CM_PLLD_LOADDSI1;
	}

	if (dsi1_cprman_read(CM_LOCK) == 0) {
		return -1;
	}

	if (port == 0) {
		/*
		 * DSI0's HS clock comes from PLLA (Linux's layout too):
		 * PLLD's fixed VCO only offers /1 or /2 around DSI rates
		 * — 1 Gbps or 500 Mbps for this panel's nominal 600 Mbps
		 * — and the hfp fixup that pays back a 67%-fast bit clock
		 * pushes the timings far outside what the panel bridge
		 * tolerates.  PLLA is free, so retune its VCO and hit the
		 * nominal HS rate exactly: nominal timings, no fixup.
		 */
		uint32_t pix_clk_div;
		uint32_t vco_actual;
		uint64_t pll_clock;

		if (mode->lanes == 0 || mode->pixel_clock_hz == 0) {
			return -1;
		}
		pix_clk_div = 24U / mode->lanes;
		if (pix_clk_div == 0) {
			return -1;
		}
		pll_clock = (uint64_t)mode->pixel_clock_hz * pix_clk_div;
		if (pll_clock < 10000000ULL || pll_clock > 1600000000ULL) {
			return -1;
		}

		/* Largest channel divider that keeps the VCO at or below
		 * 1.6 GHz — inside the no-FB-prediv band, so the
		 * firmware's PLLA analog setup stays valid as-is. */
		phy_divider = (uint32_t)(1600000000ULL / pll_clock);
		if (phy_divider < 1U) phy_divider = 1U;
		if (phy_divider > 255U) phy_divider = 255U;

		vco_actual = _plla_set_vco((uint32_t)pll_clock * phy_divider);
		if (vco_actual == 0) {
			return -1;
		}

		adj->width = mode->width;
		adj->height = mode->height;
		adj->hfp = mode->hfp;
		adj->hsw = mode->hsw;
		adj->hbp = mode->hbp;
		adj->vfp = mode->vfp;
		adj->vsw = mode->vsw;
		adj->vbp = mode->vbp;
		adj->hs_clock_hz = vco_actual / phy_divider;
		adj->pixel_clock_hz = adj->hs_clock_hz / pix_clk_div;
		adj->pix_clk_divider = pix_clk_div;

		_pll_dsi_set_divider(CM_PLLA, a2w_dsi, hold_bit, load_bit,
				phy_divider);

		/*
		 * CM_DSI0HSCK muxes the DSI0 HS clock between PLLA_DSI0
		 * (bit0=0) and PLLD_DSI0 (bit0=1) — clk-bcm2835.c parents
		 * {"plla_dsi0", "plld_dsi0"}.  The mux MUST follow the PLL
		 * channel actually programmed, or the PHY references a
		 * dead channel: no byte clock, a dead dsi0p pixel clock,
		 * a clockless PV0 and an HVS channel stuck in INIT.
		 */
		dsi1_cprman_write(CM_DSI0HSCK, 0);
	} else {
		/*
		 * DSI1's HS clock is hardwired to PLLD_DSI1 (no HSCK
		 * mux) and PLLD's VCO must not move (core/per consumers):
		 * integer-divide the fixed VCO and pay the rate error
		 * back through the vc4 hfp fixup (uconsole-proven).
		 */
		if ((dsi1_cprman_read(CM_LOCK) & CM_LOCK_FLOCKD) == 0) {
			return -1;
		}

		vco_hz = _plld_vco_hz();
		if (_mode_fixup(mode, vco_hz, adj) != 0) {
			return -1;
		}

		/* Round to nearest integer divider, clamp to 1..255. */
		phy_divider = (vco_hz + (adj->hs_clock_hz / 2U)) /
				adj->hs_clock_hz;
		if (phy_divider < 1U) phy_divider = 1U;
		if (phy_divider > 255U) phy_divider = 255U;

		_pll_dsi_set_divider(CM_PLLD, a2w_dsi, hold_bit, load_bit,
				phy_divider);
	}

	/*
	 * DSI escape clock: source dsiX_ddr2 (mux index 6), target
	 * 100 MHz — what vc4_dsi.c requests via clk_set_rate(escape,
	 * 100 MHz) and what every escape-domain constant assumes.
	 *
	 * The ddr2 tap is the HS bit clock divided by 4, as documented.
	 * (An early gen4 TCNT measurement seemed to show /2 — 200 MHz
	 * out of a div-2.5 generator — but that run was computed off a
	 * PLLD VCO under-read by 2x thanks to the missed FB prediv; with
	 * the VCO read correctly the same measurement is exactly /4.)
	 *
	 * CM_*DIV registers hold the divider in 12.12 fixed point
	 * REGARDLESS of the clock's int_bits/frac_bits — those only say
	 * which bits are wired.  dsiXe wires int_bits=4, frac_bits=8:
	 * mask the unused low 4 fraction bits.
	 */
	esc_parent_hz = adj->hs_clock_hz / 4U;
	if (esc_parent_hz == 0) {
		esc_parent_hz = 1;
	}
	esc_div_code = (uint32_t)((((uint64_t)esc_parent_hz << 12) +
			(DSI1_ESC_CLOCK_HZ / 2U)) / DSI1_ESC_CLOCK_HZ);
	esc_div_code &= ~0xFU;
	if (esc_div_code < (1U << 12)) {
		esc_div_code = 1U << 12;
	}
	if (esc_div_code > 0xFFF0U) {
		esc_div_code = 0xFFF0U;
	}

	if (_cm_set(cm_ectl, cm_ediv,
			CM_SRC_DSI_DDR2, esc_div_code,
			(esc_div_code & 0xfffU) != 0, 1) < 0) {
		/*
		 * dsiX_ddr2-sourced generator never went BUSY: fall
		 * back to XOSC (divider 1.0).  All escape timings were
		 * computed for 100 MHz, so at XOSC rate every LP period
		 * only stretches — longer than spec minimums in every
		 * case, i.e. safe, just slower.  Better a slow escape
		 * clock than a dead one (dead = no LP TX, no bus
		 * turnaround, ever).
		 */
		_dsi1e_fallback = 1;
		if (_cm_set(cm_ectl, cm_ediv,
				CM_SRC_OSC, 1U << 12, 0, 1) < 0) {
			return -1;
		}
	}

	/*
	 * DSI pixel clock: parent must be `dsiX_byte` (mux index 8),
	 * the DSI PHY's byte clock (HS / 8).  Any other index (in
	 * particular 0=gnd) leaves the pixelvalve without a clock and
	 * no pixels reach the DSI serializer.  The pixel generator has
	 * int_bits=frac_bits=0 so the DIV register is a no-op.
	 */
	if (_cm_set(cm_pctl, cm_pdiv, CM_SRC_DSI_BYTE, 0, 0, 0) < 0) {
		return -1;
	}

	return 0;
}

/*
 * bcm2835_measure_tcnt_mux(): route the selected clock into the TCNT
 * counter, gate it for CM_OSCCOUNT XOSC cycles (1 ms), and read back
 * the edge count.  count * 1000 = Hz.  This measures the clock the
 * hardware actually generates, not what the control registers claim.
 * mux values: 19 = DSI1E, 13 = DSI1P.
 */
uint32_t bcm283x_dsi1_measure_hz(uint32_t tcnt_mux) {
	uint32_t count;
	int spin;

	dsi1_cprman_write(CM_TCNTCTL, CM_KILL);
	dsi1_cprman_write(CM_TCNTCTL,
			(tcnt_mux & CM_SRC_MASK) |
			((tcnt_mux >> 4) << CM_TCNT_SRC1_SHIFT));
	/* 1 ms gate on whichever crystal this SoC has. */
	dsi1_cprman_write(CM_OSCCOUNT, dsi1_xosc_hz() / 1000U);

	bcm283x_dsi1_mdelay(1);

	/* Finish off whatever is left of OSCCOUNT. */
	for (spin = 0; spin < 100000; spin++) {
		if (dsi1_cprman_read(CM_OSCCOUNT) == 0) {
			break;
		}
		bcm283x_dsi1_udelay(1);
	}
	if (dsi1_cprman_read(CM_OSCCOUNT) != 0) {
		dsi1_cprman_write(CM_TCNTCTL, 0);
		return 0;
	}

	/* Wait for BUSY to clear. */
	for (spin = 0; spin < 100000; spin++) {
		if ((dsi1_cprman_read(CM_TCNTCTL) & CM_BUSY) == 0) {
			break;
		}
		bcm283x_dsi1_udelay(1);
	}
	if (dsi1_cprman_read(CM_TCNTCTL) & CM_BUSY) {
		dsi1_cprman_write(CM_TCNTCTL, 0);
		return 0;
	}

	count = dsi1_cprman_read(CM_TCNTCNT);
	dsi1_cprman_write(CM_TCNTCTL, 0);
	return count * 1000U;
}
