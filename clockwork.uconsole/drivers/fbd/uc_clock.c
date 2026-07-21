#include "uc_clock.h"
#include "uc_time.h"

#include <stdint.h>

#include <ewoksys/mmio.h>

#include "uc_log.h"
#define slog uc_log

static volatile uint32_t* _cprman = 0;

/* Set when DSI1E had to fall back from PLLD_PER to XOSC. */
int uc_clock_dsi1e_fallback = 0;

void uc_clock_init(void) {
	if (_cprman == 0 && _mmio_base != 0) {
		_cprman = (volatile uint32_t*)(uintptr_t)(_mmio_base + UC_CPRMAN_OFFSET);
	}
}

uint32_t uc_cprman_read(uint32_t off) {
	uc_clock_init();
	if (_cprman == 0) {
		return 0;
	}
	return _cprman[off / 4];
}

void uc_cprman_write(uint32_t off, uint32_t val) {
	uc_clock_init();
	if (_cprman == 0) {
		return;
	}
	_cprman[off / 4] = UC_CM_PASSWORD | (val & 0x00ffffffU);
}

void uc_clock_dump(void) {
	uc_clock_init();
	if (_cprman == 0) {
		slog("[uc_clock] CPRMAN not mapped\n");
		return;
	}

	slog("[uc_clock] CM_OSCCOUNT   = 0x%08x\n", uc_cprman_read(UC_CM_OSCCOUNT));
	slog("[uc_clock] CM_PLLD       = 0x%08x\n", uc_cprman_read(UC_CM_PLLD));
	slog("[uc_clock] CM_LOCK       = 0x%08x\n", uc_cprman_read(UC_CM_LOCK));
	slog("[uc_clock] CM_DSI1ECTL   = 0x%08x\n", uc_cprman_read(UC_CM_DSI1ECTL));
	slog("[uc_clock] CM_DSI1EDIV   = 0x%08x\n", uc_cprman_read(UC_CM_DSI1EDIV));
	slog("[uc_clock] CM_DSI1PCTL   = 0x%08x\n", uc_cprman_read(UC_CM_DSI1PCTL));
	slog("[uc_clock] CM_DSI1PDIV   = 0x%08x\n", uc_cprman_read(UC_CM_DSI1PDIV));
	slog("[uc_clock] A2W_XOSC_CTRL = 0x%08x\n", uc_cprman_read(UC_A2W_XOSC_CTRL));
	slog("[uc_clock] A2W_PLLD_CTRL = 0x%08x\n", uc_cprman_read(UC_A2W_PLLD_CTRL));
	slog("[uc_clock] A2W_PLLD_ANA0 = 0x%08x\n", uc_cprman_read(UC_A2W_PLLD_ANA0));
	slog("[uc_clock] A2W_PLLD_FRAC = 0x%08x\n", uc_cprman_read(UC_A2W_PLLD_FRAC));
	slog("[uc_clock] A2W_PLLD_CORE = 0x%08x\n", uc_cprman_read(UC_A2W_PLLD_CORE));
	slog("[uc_clock] A2W_PLLD_PER  = 0x%08x\n", uc_cprman_read(UC_A2W_PLLD_PER));
	slog("[uc_clock] A2W_PLLD_DSI0 = 0x%08x\n", uc_cprman_read(UC_A2W_PLLD_DSI0));
	slog("[uc_clock] A2W_PLLD_DSI1 = 0x%08x\n", uc_cprman_read(UC_A2W_PLLD_DSI1));
}

/*
 * Program the A2W_PLLD_DSI1 integer divider and enable the channel.
 * Every A2W write must preserve the top-byte password magic which our
 * uc_cprman_write() helper does automatically. After the divider is set
 * we kick CM_PLLD_LOADDSI1 to latch it.
 */
static void _plld_dsi1_set_divider(uint32_t divider) {
	uint32_t ch;

	/* Hold the DSI1 phase during the update. */
	uc_cprman_write(UC_CM_PLLD,
			uc_cprman_read(UC_CM_PLLD) | UC_CM_PLLD_HOLDDSI1);

	ch = uc_cprman_read(UC_A2W_PLLD_DSI1);
	ch &= ~UC_A2W_PLL_DIV_MASK;
	ch |= (divider & UC_A2W_PLL_DIV_MASK);
	ch &= ~UC_A2W_PLL_CHANNEL_DISABLE;
	uc_cprman_write(UC_A2W_PLLD_DSI1, ch);

	/* Pulse LOADDSI1 to latch the new divider. */
	uc_cprman_write(UC_CM_PLLD,
			uc_cprman_read(UC_CM_PLLD) | UC_CM_PLLD_LOADDSI1);
	uc_udelay(1);
	uc_cprman_write(UC_CM_PLLD,
			uc_cprman_read(UC_CM_PLLD) & ~UC_CM_PLLD_LOADDSI1);

	/* Release the hold. */
	uc_cprman_write(UC_CM_PLLD,
			uc_cprman_read(UC_CM_PLLD) & ~UC_CM_PLLD_HOLDDSI1);
}

/*
 * CM_*CTL update:  KILL the clock, wait for BUSY to clear, rewrite the
 * source + divider, then set ENABLE. This is the same pattern that
 * clk-bcm2835.c uses when reparenting a clock.
 *
 * `div_code` is the raw CM_*DIV register value for this specific clock:
 * ALWAYS 12.12 fixed point (integer [23:12], fraction [11:0]); a clock's
 * int_bits/frac_bits only describe which of those bits are wired up.
 * `use_frac` mirrors bcm2835_clock_set_rate: CM_FRAC must be set in CTL
 * whenever any fraction bit [11:0] is non-zero.
 */
static int _cm_set(uint32_t ctl_off, uint32_t div_off,
		uint32_t src, uint32_t div_code, int use_frac, int verify_busy) {
	uint32_t ctl;
	uint32_t ctl_val;
	int spin;

	ctl = uc_cprman_read(ctl_off);
	if (ctl & UC_CM_ENABLE) {
		uc_cprman_write(ctl_off, (ctl & ~UC_CM_ENABLE) | UC_CM_KILL);
		for (spin = 0; spin < 10000; spin++) {
			if ((uc_cprman_read(ctl_off) & UC_CM_BUSY) == 0) {
				break;
			}
			uc_udelay(1);
		}
		if ((uc_cprman_read(ctl_off) & UC_CM_BUSY) != 0) {
			slog("[uc_clock] CM 0x%03x kill timeout\n", ctl_off);
			return -1;
		}
	}

	/* CM_*DIV: raw register value, integer part pre-shifted. */
	uc_cprman_write(div_off, div_code & 0xffffffU);
	/* Set SRC (bits [3:0]) + FRAC, then set ENABLE. */
	ctl_val = (src & UC_CM_SRC_MASK) | (use_frac ? UC_CM_FRAC : 0);
	uc_cprman_write(ctl_off, ctl_val);
	uc_cprman_write(ctl_off, ctl_val | UC_CM_ENABLE);

	/*
	 * BUSY = the generator is actually running. Skippable for
	 * clocks whose parent doesn't tick yet at programming time
	 * (CM_DSI1P sources dsi1_byte, which only starts with the
	 * PHY); mandatory for the escape clock, because a silently
	 * dead DSI1E kills LP transmission and bus turnaround while
	 * every HS path keeps working -- the exact symptom this
	 * driver spent rounds chasing on the panel side.
	 */
	if (verify_busy) {
		for (spin = 0; spin < 10000; spin++) {
			if (uc_cprman_read(ctl_off) & UC_CM_BUSY) {
				return 0;
			}
			uc_udelay(1);
		}
		slog("[uc_clock] CM 0x%03x never went BUSY\n", ctl_off);
		return -1;
	}
	return 0;
}

int uc_clock_bringup_dsi1(uint32_t target_hs_hz) {
	uint32_t ndiv;
	uint32_t fdiv;
	uint32_t xosc_hz = 54000000U;    /* BCM2711 XOSC (CM4) */
	uint32_t vco_hz;
	uint32_t phy_divider;

	uc_clock_init();
	if (uc_cprman_read(UC_CM_LOCK) == 0) {
		slog("[uc_clock] CPRMAN not readable\n");
		return -1;
	}

	if ((uc_cprman_read(UC_CM_LOCK) & UC_CM_LOCK_FLOCKD) == 0) {
		slog("[uc_clock] PLLD not locked (CM_LOCK=0x%08x)\n",
				uc_cprman_read(UC_CM_LOCK));
		return -1;
	}

	/*
	 * Read PLLD's VCO frequency:
	 *   VCO = XOSC * (NDIV + FDIV / 2^20)
	 * NDIV lives in A2W_PLLD_CTRL[9:0], FDIV in A2W_PLLD_FRAC[19:0].
	 * The password magic in bits 31..24 doesn't matter on read.
	 */
	{
		uint32_t ctl = uc_cprman_read(UC_A2W_PLLD_CTRL);
		uint32_t pdiv = (ctl & UC_A2W_PLL_CTRL_PDIV_MASK) >>
				UC_A2W_PLL_CTRL_PDIV_SHIFT;
		uint64_t vco;
		ndiv = ctl & UC_A2W_PLL_CTRL_NDIV_MASK;
		fdiv = uc_cprman_read(UC_A2W_PLLD_FRAC) & 0xfffffU;
		/*
		 * bcm2835_pll_get_rate():  rate = xosc * (ndiv + fdiv/2^20) / pdiv
		 * PLLD on BCM2711 normally has PDIV=1 (VCO ~= 3 GHz), but if VC
		 * firmware boots with PDIV != 1 we MUST honour it or we'll pick
		 * a PLLD_DSI1 divider that's off by pdiv and the panel will
		 * never lock on the HS clock.
		 */
		vco = (uint64_t)xosc_hz * ndiv;
		vco += ((uint64_t)xosc_hz * fdiv) >> 20;
		if (pdiv != 0) {
			vco /= pdiv;
		}
		vco_hz = (uint32_t)vco;
		slog("[uc_clock] PLLD CTL=0x%08x ndiv=%u pdiv=%u fdiv=0x%x VCO=%uHz\n",
				ctl, ndiv, pdiv, fdiv, vco_hz);
	}
	if (vco_hz == 0 || target_hs_hz == 0) {
		return -1;
	}

	/* Round to nearest integer divider, clamp to 1..255. */
	phy_divider = (vco_hz + (target_hs_hz / 2U)) / target_hs_hz;
	if (phy_divider < 1U) phy_divider = 1U;
	if (phy_divider > 255U) phy_divider = 255U;

	slog("[uc_clock] PLLD ndiv=%u fdiv=0x%x VCO=%uHz target=%uHz div=%u\n",
			ndiv, fdiv, vco_hz, target_hs_hz, phy_divider);

	_plld_dsi1_set_divider(phy_divider);

	/*
	 * DSI1 escape clock: source PLLD_PER, target 100 MHz — that is
	 * what vc4_dsi.c requests via clk_set_rate(escape, 100 MHz) and
	 * every escape-domain constant assumes (ESC_TIME_NS=10, LPDT
	 * timings, DLT7 wakeup...).  PLLD_PER is *not* fixed at 500 MHz:
	 * Pi4 firmware runs PLLD at 3 GHz with A2W_PLLD_PER divider 4 →
	 * 750 MHz.  Compute the actual PLLD_PER rate from the VCO and the
	 * A2W channel divider, then program the DSI1E 4.8 fixed-point
	 * fractional divider to land on 100 MHz exactly (750 MHz → 7.5).
	 */
	{
		uint32_t per_div = uc_cprman_read(UC_A2W_PLLD_PER) &
				UC_A2W_PLL_DIV_MASK;
		uint32_t plld_per_hz;
		uint32_t esc_div_code;

		if (per_div == 0) {
			per_div = 1;
		}
		plld_per_hz = vco_hz / per_div;

		/*
		 * CM_*DIV registers hold the divider in 12.12 fixed point
		 * (integer [23:12], fraction [11:0]) REGARDLESS of the
		 * clock's int_bits/frac_bits — those only say which bits
		 * are wired.  bcm2835_clock_choose_div() computes
		 * parent<<12/rate and set_rate writes that value to the
		 * register unmodified; the >>(12-frac_bits) shift in
		 * rate_from_divisor is only for doing math on it, NOT the
		 * register layout.  Writing a 4.8-shifted code here made
		 * the hardware see 7.5 as 0.469 and the escape clock ran
		 * at ~640 MHz instead of 100 MHz (measured via TCNT).
		 * dsi1e wires int_bits=4, frac_bits=8: mask the unused
		 * low 4 fraction bits.
		 */
		esc_div_code = (uint32_t)((((uint64_t)plld_per_hz << 12) +
				(UC_DSI_ESC_CLOCK_HZ / 2U)) / UC_DSI_ESC_CLOCK_HZ);
		esc_div_code &= ~0xFU;
		if (esc_div_code < (1U << 12)) {
			esc_div_code = 1U << 12;
		}
		if (esc_div_code > 0xFFF0U) {
			esc_div_code = 0xFFF0U;
		}

		slog("[uc_clock] PLLD_PER=%uHz esc div=0x%03x\n",
				plld_per_hz, esc_div_code);

		if (_cm_set(UC_CM_DSI1ECTL, UC_CM_DSI1EDIV,
				UC_CM_SRC_PLLD_PER, esc_div_code,
				(esc_div_code & 0xfffU) != 0, 1) < 0) {
			/*
			 * PLLD_PER-sourced generator never went BUSY: fall
			 * back to XOSC (54 MHz, divider 1.0). All escape
			 * timings were computed for 100 MHz, so at 54 MHz
			 * every LP period stretches ~1.85x -- longer than
			 * spec minimums in every case, i.e. safe, just
			 * slower. Better a slow escape clock than a dead
			 * one (dead = no LP TX, no bus turnaround, ever).
			 */
			uc_clock_dsi1e_fallback = 1;
			if (_cm_set(UC_CM_DSI1ECTL, UC_CM_DSI1EDIV,
					UC_CM_SRC_OSC, 1U << 12, 0, 1) < 0) {
				return -1;
			}
		}
	}

	/*
	 * DSI1 pixel clock: parent must be `dsi1_byte` (mux index 8 in
	 * bcm2835_clock_dsi1_parents), i.e. the DSI PHY's byte clock.
	 * Any other index (in particular 0=gnd) leaves PV1 without a
	 * clock and no pixels reach the DSI serializer.
	 * DSI1P has int_bits=frac_bits=0 so CM_DSI1PDIV is a no-op;
	 * div_code is irrelevant but we pass 0 for clarity.
	 */
	if (_cm_set(UC_CM_DSI1PCTL, UC_CM_DSI1PDIV,
			UC_CM_SRC_DSI1_BYTE, 0, 0, 0) < 0) {
		return -1;
	}

	slog("[uc_clock] DSI1 clocks up: CM_LOCK=0x%08x CM_DSI1E=0x%08x CM_DSI1P=0x%08x\n",
			uc_cprman_read(UC_CM_LOCK),
			uc_cprman_read(UC_CM_DSI1ECTL),
			uc_cprman_read(UC_CM_DSI1PCTL));
	return 0;
}

/*
 * bcm2835_measure_tcnt_mux(): route the selected clock into the TCNT
 * counter, gate it for CM_OSCCOUNT XOSC cycles (1ms at the BCM2711's
 * 54 MHz crystal), and read back the edge count. count * 1000 = Hz.
 * This measures the clock the hardware actually generates, not what
 * the control registers claim.
 */
uint32_t uc_clock_measure_hz(uint32_t tcnt_mux) {
	uint32_t count;
	int spin;

	uc_clock_init();
	if (_cprman == 0) {
		return 0;
	}

	uc_cprman_write(UC_CM_TCNTCTL, UC_CM_KILL);
	uc_cprman_write(UC_CM_TCNTCTL,
			(tcnt_mux & UC_CM_SRC_MASK) |
			((tcnt_mux >> 4) << UC_CM_TCNT_SRC1_SHIFT));
	uc_cprman_write(UC_CM_OSCCOUNT, 54000U);   /* 1ms at 54 MHz XOSC */

	uc_mdelay(1);

	/* Finish off whatever is left of OSCCOUNT. */
	for (spin = 0; spin < 100000; spin++) {
		if (uc_cprman_read(UC_CM_OSCCOUNT) == 0) {
			break;
		}
		uc_udelay(1);
	}
	if (uc_cprman_read(UC_CM_OSCCOUNT) != 0) {
		slog("[uc_clock] TCNT mux %u: OSCCOUNT stuck\n", tcnt_mux);
		uc_cprman_write(UC_CM_TCNTCTL, 0);
		return 0;
	}

	/* Wait for BUSY to clear. */
	for (spin = 0; spin < 100000; spin++) {
		if ((uc_cprman_read(UC_CM_TCNTCTL) & UC_CM_BUSY) == 0) {
			break;
		}
		uc_udelay(1);
	}
	if (uc_cprman_read(UC_CM_TCNTCTL) & UC_CM_BUSY) {
		slog("[uc_clock] TCNT mux %u: BUSY stuck\n", tcnt_mux);
		uc_cprman_write(UC_CM_TCNTCTL, 0);
		return 0;
	}

	count = uc_cprman_read(UC_CM_TCNTCNT);
	uc_cprman_write(UC_CM_TCNTCTL, 0);
	return count * 1000U;
}
