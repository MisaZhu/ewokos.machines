#include "uc_clock.h"
#include "uc_time.h"

#include <stdint.h>

#include <ewoksys/mmio.h>

/* Silent operation -- slog is stubbed to a no-op. */
#define slog(...) ((void)0)

static volatile uint32_t* _cprman = 0;

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
 * `frac_bits` is the CM_*DIV encoding for this specific clock; the
 * integer N is stored as (N << frac_bits) at bit 0 of div_reg
 * (see clk-bcm2835.c line 991 `div >>= 12 - frac_bits`).
 */
static int _cm_set(uint32_t ctl_off, uint32_t div_off,
		uint32_t src, uint32_t divider, uint32_t frac_bits) {
	uint32_t ctl;
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

	/* CM_*DIV: integer N encoded at bit 0 shifted by frac_bits. */
	uc_cprman_write(div_off, (divider & 0xfffU) << frac_bits);
	/* Set SRC (bits [3:0]) with CM_SRC_MASK, then set ENABLE. */
	uc_cprman_write(ctl_off, (src & UC_CM_SRC_MASK));
	uc_cprman_write(ctl_off, (src & UC_CM_SRC_MASK) | UC_CM_ENABLE);
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
	 * DSI1 escape clock: source PLLD_PER (500 MHz typical), divide by 5
	 * to get ~100 MHz which is what vc4 assumes (ESC_TIME_NS=10).
	 * DSI1E CM_DIV encoding is int.frac with frac_bits=8.
	 */
	if (_cm_set(UC_CM_DSI1ECTL, UC_CM_DSI1EDIV,
			UC_CM_SRC_PLLD_PER, 5, UC_CM_DIV_FRAC_DSI1E) < 0) {
		return -1;
	}

	/*
	 * DSI1 pixel clock: parent must be `dsi1_byte` (mux index 8 in
	 * bcm2835_clock_dsi1_parents), i.e. the DSI PHY's byte clock.
	 * Any other index (in particular 0=gnd) leaves PV1 without a
	 * clock and no pixels reach the DSI serializer.
	 * DSI1P has int_bits=frac_bits=0 so CM_DSI1PDIV is a no-op;
	 * frac_bits argument is irrelevant but we pass 0 for clarity.
	 */
	if (_cm_set(UC_CM_DSI1PCTL, UC_CM_DSI1PDIV,
			UC_CM_SRC_DSI1_BYTE, 1, 0) < 0) {
		return -1;
	}

	slog("[uc_clock] DSI1 clocks up: CM_LOCK=0x%08x CM_DSI1E=0x%08x CM_DSI1P=0x%08x\n",
			uc_cprman_read(UC_CM_LOCK),
			uc_cprman_read(UC_CM_DSI1ECTL),
			uc_cprman_read(UC_CM_DSI1PCTL));
	return 0;
}
