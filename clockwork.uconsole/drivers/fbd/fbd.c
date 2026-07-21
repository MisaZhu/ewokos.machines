#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <sysinfo.h>
#include <fbd/fbd.h>
#include <graph/graph.h>
#include <arch/bcm283x/framebuffer.h>

#include "uc_time.h"
#include "uc_backlight.h"
#include "uc_panel.h"
#include "uc_dsi.h"
#include "uc_clock.h"
#include "uc_fb.h"
#include "uc_cwu50.h"
#include "uc_hvs.h"
#include "uc_pv.h"
#include "uc_power.h"
#include "uc_pmu.h"
#include "uc_log.h"

/*
 * uConsole (ClockworkPi CM4) framebuffer daemon.
 *
 * Panel is a MIPI DSI "cw,cwu50" module.  This daemon drives the
 * whole path from cold: PLLD_DSI1 clocks -> DSI1 PHY + controller ->
 * panel reset -> OCP8178 backlight -> cwu50 DCS init table ->
 * DSI video mode.
 *
 * Scan-out targets a physical address in the Pi4 reserved FB window
 * (0x3c000000..0x40000000 on a 1GB CM4). The kernel's
 * check_mem_map_arch() only lets userspace map RAM addresses >=
 * PI4_FB_LOW_BASE (0x3c100000), so anything below that (e.g. the old
 * 0xC00000 that fbd.workaround used) will silently fail SYS_MEM_MAP
 * and take the whole fbd process down with init()==-1.
 *
 * REAL HARDWARE: config.txt MUST set gpu_mem=16. The VC firmware
 * reserves gpu_mem MB at the top of the first 1GB for its own heap;
 * the Pi4 default (76MB) starts the carve at 0x3B400000 which sits
 * right on top of this framebuffer, so firmware and fbd would trample
 * each other's memory. gpu_mem=16 moves the carve to 0x3F000000+.
 */
#define UCONSOLE_FB_PHYS_BASE    0x3c100000U

static const char* _conf_file = "";
static fbinfo_t _fb_info;

static uint16_t rgb565_from_u32(uint32_t s) {
	uint8_t r = (uint8_t)((s >> 16) & 0xff);
	uint8_t g = (uint8_t)((s >> 8) & 0xff);
	uint8_t b = (uint8_t)(s & 0xff);
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint32_t blt32_pitch(const fbinfo_t* fbinfo, const graph_t* g) {
	uint32_t bytes_per_pixel = fbinfo->depth / 8;
	uint8_t* dst = (uint8_t*)(uintptr_t)(fbinfo->pointer +
			fbinfo->yoffset * fbinfo->pitch +
			fbinfo->xoffset * bytes_per_pixel);
	const uint32_t* src = g->buffer;
	uint32_t row_bytes = (uint32_t)g->w * bytes_per_pixel;
	uint32_t total_bytes = (uint32_t)g->h * row_bytes;

	if (fbinfo->pitch == row_bytes) {
		memcpy(dst, src, total_bytes);
		return total_bytes;
	}

	for (int32_t y = 0; y < g->h; ++y) {
		uint8_t* dst_row = dst + y * fbinfo->pitch;
		const uint8_t* src_row = (const uint8_t*)(src + y * g->w);
		memcpy(dst_row, src_row, row_bytes);
	}
	return total_bytes;
}

static uint32_t blt16_pitch(const fbinfo_t* fbinfo, const graph_t* g) {
	uint8_t* dst_base = (uint8_t*)(uintptr_t)fbinfo->pointer +
			fbinfo->yoffset * fbinfo->pitch +
			fbinfo->xoffset * 2;

	for (int32_t y = 0; y < g->h; ++y) {
		uint16_t* dst_row = (uint16_t*)(dst_base + y * fbinfo->pitch);
		const uint32_t* src_row = g->buffer + y * g->w;

		for (int32_t x = 0; x < g->w; ++x) {
			dst_row[x] = rgb565_from_u32(src_row[x]);
		}
	}
	return (uint32_t)g->w * (uint32_t)g->h * 2U;
}

static uint32_t flush(const fbinfo_t* fbinfo, const graph_t* g) {
	const fbinfo_t* phy = fbinfo != NULL ? fbinfo : &_fb_info;

	if (phy == NULL || phy->pointer == 0) {
		return 0;
	}

	if (phy->depth != 32 && phy->depth != 16) {
		return 0;
	}

	if (phy->depth == 16) {
		return blt16_pitch(phy, g);
	}

	/* Zero-copy fast path: rendering already targets the framebuffer. */
	if ((uintptr_t)phy->pointer == (uintptr_t)g->buffer) {
		return (uint32_t)g->w * (uint32_t)g->h * 4U;
	}
	return blt32_pitch(phy, g);
}

static fbinfo_t* get_info(void) {
	return &_fb_info;
}

/*
 * Fatal bring-up failure: dump every relevant register bank into the
 * log, flush the log to /fbd.log on the SD card (synchronous ext2
 * write-through — survives power-off), then flag the stage on the
 * backlight.  Diagnosis happens by reading the file, not by counting
 * blinks.
 */
static int32_t fail_stage(int stage) {
	uc_log("[fbd] FATAL: stage %d failed\n", stage);
	uc_clock_dump();
	uc_dsi_dump();
	uc_log_save();
	uc_backlight_panic((uint32_t)stage);
	return -1;
}

static int32_t map_scanout_buffer(uint32_t w, uint32_t h, uint32_t dep) {
	uint32_t bpp = dep / 8;
	uint32_t pitch = w * bpp;
	uint32_t size = pitch * h;
	uint32_t size_max = (size + 0xfffU) & ~0xfffU;
	sys_info_t sysinfo;

	memset(&_fb_info, 0, sizeof(_fb_info));
	_fb_info.width = w;
	_fb_info.height = h;
	_fb_info.vwidth = w;
	_fb_info.vheight = h;
	_fb_info.depth = dep;
	_fb_info.pitch = pitch;
	_fb_info.phy_base = UCONSOLE_FB_PHYS_BASE;
	_fb_info.size = size;
	_fb_info.size_max = size_max;
	_fb_info.xoffset = 0;
	_fb_info.yoffset = 0;
	_fb_info.dma_id = -1;

	/*
	 * Match raspix' bcm283x_fb_init(): the scan-out user-space vaddr
	 * lives *past* the sys_dma window. SYS_P2V hands back a kernel
	 * P2V vaddr which SYS_MEM_MAP cannot legally re-use from userland,
	 * so we have to pick a fresh user vaddr the kernel can map for us.
	 */
	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
	_fb_info.pointer = sysinfo.sys_dma.v_base + sysinfo.sys_dma.size;

	if (syscall3(SYS_MEM_MAP,
			(ewokos_addr_t)_fb_info.pointer,
			(ewokos_addr_t)_fb_info.phy_base,
			(ewokos_addr_t)_fb_info.size_max) == 0) {
		_fb_info.pointer = 0;
		return -1;
	}
	return 0;
}

/*
 * Paint the whole scan-out buffer solid red.  If the pixel pipeline is
 * live, the panel goes red; if it isn't, the buffer is still primed so
 * whatever we bring up later has a known ground truth to display.
 */
static void fill_red(const fbinfo_t* fbi) {
	uint8_t* base;
	uint32_t bpp;
	uint32_t x;
	uint32_t y;

	if (fbi == NULL || fbi->pointer == 0) {
		return;
	}
	bpp = fbi->depth / 8U;
	if (bpp != 2U && bpp != 4U) {
		return;
	}

	base = (uint8_t*)(uintptr_t)fbi->pointer;
	for (y = 0; y < fbi->height; y++) {
		uint8_t* row = base + y * fbi->pitch;
		if (bpp == 2U) {
			uint16_t* p16 = (uint16_t*)row;
			for (x = 0; x < fbi->width; x++) {
				p16[x] = 0xF800U;   /* RGB565 red */
			}
		} else {
			uint32_t* p32 = (uint32_t*)row;
			for (x = 0; x < fbi->width; x++) {
				p32[x] = 0x00FF0000U;
			}
		}
	}
}

/*
 * Splash callback that paints the graph buffer solid red.  fbd_run()
 * fills its SHM using this and then hands the SHM to our flush() which
 * memcpy's it into the scan-out. If the display path is alive at all,
 * the panel goes red.
 */
static void red_splash(graph_t* g, const char* logo_fname) {
	uint32_t i;
	uint32_t n;
	uint32_t* p;

	(void)logo_fname;
	if (g == NULL || g->buffer == NULL) {
		return;
	}
	p = (uint32_t*)g->buffer;
	n = (uint32_t)g->w * (uint32_t)g->h;
	for (i = 0; i < n; i++) {
		p[i] = 0xFFFF0000U;   /* ARGB opaque red */
	}
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
	if (w == 0) {
		w = UC_PANEL_WIDTH;
	}
	if (h == 0) {
		h = UC_PANEL_HEIGHT;
	}
	if (dep != 16 && dep != 32) {
		dep = 32;
	}

	mmio_map();
	if (_mmio_base == 0) {
		return -1;
	}

	/* GPIO-only early setup: claim the reset pin at its probe-time
	 * level (physical LOW = deasserted for the active-low cwu50
	 * reset), and light the backlight. The actual reset PULSE must
	 * wait until the DSI host is in LP-11, matching DRM's ordering
	 * (host pre_enable → panel prepare). */
	uc_time_init();
	uc_panel_probe();
	uc_backlight_init();
	uc_backlight_set(UC_BACKLIGHT_DEFAULT);

	/*
	 * PANEL RAILS FIRST — before ANY DSI PHY activity.
	 *
	 * JD9365DA-H3 datasheet §9.5.2 power-on sequence: VCI/IOVCC etc.
	 * must be up BEFORE the MIPI lines go LP-11 (tMIPI-ON is bounded
	 * by tRPWIRES), then >=5ms (tRPWIRES) before the HW reset pulse.
	 * Driving LP-11 + continuous HS clock into an UNPOWERED DDIC
	 * (what the old order did: PHY at stage 4, rails after stage 5)
	 * parasitically powers the chip through its ESD diodes, so the
	 * real rail arrival never triggers a clean POR — the panel comes
	 * up latched in an undefined state, deaf to reset, DCS and BTA.
	 * Linux never hits this: the AXP regulators are regulator-always-on
	 * from early boot, long before vc4 touches the lanes.
	 *
	 * uc_pmu_display_power() ends with a 20ms settle, which covers
	 * tRPWIRES before the PHY starts driving in uc_dsi_bringup().
	 * On failure the rails may still be on from PMIC defaults, so
	 * flag it (slow 11 marker, now at the FRONT of the sequence)
	 * and continue.
	 */
	if (uc_pmu_display_power() != 0) {
		uc_log("[fbd] WARN: AXP223 display rails unverified over I2C\n");
		uc_backlight_blink(11);
	}

	/*
	 * Bare-metal DSI/HVS/PV path only. We intentionally do NOT fall
	 * back to bcm283x_fb_init() / VC firmware mailbox: on uConsole the
	 * VC firmware has no cwu50 panel config, so a mailbox-allocated FB
	 * would land somewhere that the DSI controller is never told to
	 * scan out, and everything downstream would silently paint into
	 * dead memory.
	 */
	uc_clock_init();
	uc_dsi_init();

	/*
	 * Firmware power domains: in EVERY bcm27xx dtsi &dsi1 carries no
	 * power-domains property, so Linux never performs a firmware
	 * domain transition for DSI1 — vc4's pm_runtime only gates
	 * clocks. Do NOT power-cycle here (a forced OFF->ON edge is
	 * something upstream never generates); just log the state and
	 * enable only if the firmware reports OFF.
	 */
	/*
	 * Blink-code readout (log also goes to kout/UART + /fbd.log, but
	 * the backlight is the channel that always works):
	 *   N slow blinks + 1s gap   = stage N passed
	 *   N fast blinks forever    = stage N FAILED (halted)
	 * Stages:
	 *   1 = firmware power domains ON
	 *   2 = DSI1 clocks (PLLD_DSI1/escape/pixel) programmed
	 *   3 = DSI1 register bus alive (ID reads "dsi")
	 *   4 = DSI PHY bringup + all lanes in LP-11 STOP
	 *   5 = scan-out buffer mapping
	 *   6 = panel reset + cwu50 DCS init
	 *   7 = HVS ch1 video engine reached RUN
	 *   8 = HVS frame counter advancing
	 * Extra slow markers (position in the sequence disambiguates):
	 *   FIRST (before everything): 11 = AXP223 display rails
	 *                 unverified over I2C (rails now go on at the
	 *                 very start, before any DSI activity)
	 *   after 2:  9 = TCNT says DSI1E escape clock NOT ticking
	 *            12 = escape clock fell back from PLLD_PER to XOSC
	 *   after 4: 10 = TCNT says DSI1P/byte clock NOT ticking --
	 *                 the PHY ANALOG PLL is dead even though the
	 *                 digital side reports LP-11 (decisive!)
	 * (stage-pass blinks 1..5 are commented out; stage 6 pass now
	 *  blinks 1 as a single "init table sent" marker)
	 *   after 6:  9 = panel ANSWERED DCS 0x0A: sleep-out+display-on
	 *            10 = panel answered but NOT sleep-out/display-on
	 *            11 = BTA never granted by controller (ret=-1)
	 *            12 = BTA turnaround done, panel stayed silent (-2)
	 * then the PHYSICAL-LAYER VERDICT (only after 11/12):
	 *            13 = TA_TO: turnaround timed out, far end never
	 *                 drove the lines (panel electrically absent)
	 *            14 = LP contention seen: panel IS driving the LP
	 *                 lines (electrically present, protocol issue)
	 *            15 = neither bit: BTA never initiated
	 *
	 * MATRIX PHASE (after the marker above): four trials, blink N
	 * (1=normal+HS 2=inverted+HS 3=normal+LP 4=inverted+LP) then
	 * reset + full init + DCS 0x23 All-Pixels-On + 5s pause.  A
	 * WHITE screen right after trial N identifies the working
	 * reset-polarity/command-mode combination.
	 */
	{
		int st = uc_power_domain_get(UC_PWR_DOMAIN_DSI1);
		uc_log("[fbd] DSI1 domain state=%d\n", st);
		if (st == 0) {
			if (uc_power_domain_set(UC_PWR_DOMAIN_DSI1, 1) != 0) {
				return fail_stage(1);
			}
			uc_mdelay(20);
		}
		st = uc_power_domain_get(UC_PWR_DOMAIN_VIDEO_SCALER);
		uc_log("[fbd] VIDEO_SCALER domain state=%d\n", st);
		if (st == 0) {
			uc_power_domain_set(UC_PWR_DOMAIN_VIDEO_SCALER, 1);
			uc_mdelay(20);
		}
	}
	//uc_backlight_blink(1);

	if (uc_clock_bringup_dsi1(UC_DSI_HS_CLOCK_HZ) != 0) {
		return fail_stage(2);
	}
	//uc_backlight_blink(2);
	if (uc_clock_dsi1e_fallback) {
		uc_log("[fbd] WARN: DSI1E refused PLLD_PER, running from XOSC\n");
		uc_backlight_blink(12);
	}
	/*
	 * TCNT counts REAL edges of the selected clock inside CPRMAN
	 * (same counter clk-bcm2835 uses), so this is ground truth for
	 * "is the escape clock actually ticking", independent of what
	 * the mux registers claim.
	 */
	{
		uint32_t hz = uc_clock_measure_hz(UC_TCNT_MUX_DSI1E);
		uc_log("[fbd] measured DSI1E = %u Hz (want ~%u)\n",
				hz, UC_DSI_ESC_CLOCK_HZ);
		if (hz == 0) {
			uc_backlight_blink(9);
		}
	}

	if (uc_dsi_alive() != 0) {
		/* ID register wrong: register bus/power issue, not timing. */
		return fail_stage(3);
	}
	//uc_backlight_blink(3);

	if (uc_dsi_bringup() != 0 || uc_dsi_lanes_stopped() != 0) {
		/* PHY refused to drive LP-11 on the data lanes. */
		return fail_stage(4);
	}
	//uc_backlight_blink(4);
	/*
	 * DSI1P sources dsi1_byte, which only exists if the DSI PHY's
	 * ANALOG PLL is really running. A sane measurement here is the
	 * first direct proof of analog-side life (expected byte clock
	 * = HS/8).  Zero => slow 10-blink: the analog block is dead no
	 * matter what the digital status bits say.
	 */
	{
		uint32_t hz = uc_clock_measure_hz(UC_TCNT_MUX_DSI1P);
		uc_log("[fbd] measured DSI1P/byte = %u Hz (want ~%u)\n",
				hz, UC_DSI_HS_CLOCK_HZ / 8U);
		if (hz == 0) {
			uc_backlight_blink(10);
		}
	}

	if (map_scanout_buffer(w, h, dep) != 0) {
		return fail_stage(5);
	}
	//uc_backlight_blink(5);

	/*
	 * Panel reset pulse + DCS init must run BEFORE we let HVS/PV
	 * start pushing pixels: like upstream, DISP0_CTRL stays 0 all
	 * the way through panel init (vc4 writes it exactly once, in
	 * the bridge enable step after pre_enable). The reset pulse
	 * sits here (not at process start) so the panel comes out of
	 * reset seeing LP-11 on the lanes, exactly like DRM's host
	 * pre_enable → panel prepare ordering.
	 *
	 * (Panel rails were already enabled at the very top of init(),
	 * per the JD9365DA-H3 §9.5.2 requirement: power → LP-11 →
	 * >=5ms → reset → init.)
	 */
	uc_panel_reset();
	if (uc_cwu50_init() != 0) {
		/*
		 * Non-zero = number of DCS commands whose TXPKT1_DONE never
		 * fired. The panel cannot have been initialised, so no red
		 * screen will follow -- flag it instead of continuing.
		 */
		return fail_stage(6);
	}
	uc_backlight_blink(1);

	/*
	 * PANEL-SIDE probe of the DCS init: read DCS 0x0A (Get Power
	 * Mode) back over the same link.  TXPKT1_DONE only shows our
	 * controller finished serialising a packet; this readback is
	 * the first probe that requires the panel to answer.
	 *
	 * The read is tried in HS then LP; on failure the ENTIRE init
	 * table is retried once in LP mode and the read repeated.  Not
	 * fatal either way — some DDICs never answer BTA — but every
	 * outcome is logged, including the LP contention error bits
	 * which tell a shorted/fighting LP pair apart from a simply
	 * silent panel.
	 */
	{
		uint8_t pm = 0;
		int got = uc_dsi_dcs_read(0x0A, &pm, 1, 0);
		uc_log("[fbd] DCS 0x0A read HS: ret=%d pm=0x%x\n", got, pm);
		if (got < 0 || (pm & 0x14U) != 0x14U) {
			got = uc_dsi_dcs_read(0x0A, &pm, 1, 1);
			uc_log("[fbd] DCS 0x0A read LP: ret=%d pm=0x%x\n", got, pm);
		}
		if (got >= 0) {
			uc_log("[fbd] panel ANSWERED BTA, power mode 0x%x (%s)\n",
					pm, (pm & 0x14U) == 0x14U ?
					"sleep-out + display-on" :
					"NOT sleep-out/display-on");
			/* Slow 9 = confirmed displaying; slow 10 = alive but
			 * refuses sleep-out/display-on (init table issue). */
			uc_backlight_blink((pm & 0x14U) == 0x14U ? 9 : 10);
		} else {
			/*
			 * -1 = turnaround never granted (controller side),
			 * -2 = bus handed over but panel stayed silent.
			 */
			uint32_t stat = uc_dsi1_read(UC_DSI1_INT_STAT);
			uc_log("[fbd] panel never answered BTA (ret=%d)\n", got);
			uc_log("[fbd] INT_STAT=0x%x%s%s%s\n", stat,
					(stat & UC_DSI1_INT_TA_TO) ?
						" TA_TO" : "",
					(stat & UC_DSI1_INT_ERR_CONT_LP0) ?
						" ERR_CONT_LP0" : "",
					(stat & UC_DSI1_INT_ERR_CONT_LP1) ?
						" ERR_CONT_LP1" : "");
			uc_backlight_blink(got == -1 ? 11 : 12);
			/*
			 * PHYSICAL-LAYER VERDICT from the controller's own
			 * evidence bits (W1C, accumulated since bringup):
			 *  13 = TA_TO: turnaround was ATTEMPTED and timed
			 *       out — nobody on the far end ever drove the
			 *       lines.  Panel electrically absent/dead.
			 *  14 = LP contention: something IS driving the LP
			 *       lines against us — panel electrically
			 *       present, protocol/timing issue.
			 *  15 = neither: controller never initiated the BTA.
			 */
			if (stat & UC_DSI1_INT_TA_TO) {
				uc_backlight_blink(13);
			} else if (stat & (UC_DSI1_INT_ERR_CONT_LP0 |
					UC_DSI1_INT_ERR_CONT_LP1)) {
				uc_backlight_blink(14);
			} else {
				uc_backlight_blink(15);
			}
		}
	}

	/*
	 * MATRIX DISCRIMINATOR, first run with CORRECT clocks (the
	 * escape clock ran 6.4x fast until the CM_DSI1EDIV 12.12 fix;
	 * every earlier matrix result is void).  Variables: reset
	 * polarity x command mode.  Feedback: DCS 0x23 All Pixels On —
	 * full WHITE from the DDIC itself, no video stream needed.
	 *
	 * Four trials, each announced by a blink count, then 5s of
	 * observation time:
	 *   blink 1: normal reset,   HS commands
	 *   blink 2: inverted reset, HS commands
	 *   blink 3: normal reset,   LP commands
	 *   blink 4: inverted reset, LP commands
	 * WHICHEVER trial number is followed by a WHITE screen is the
	 * working combination.  No white in any = fault beyond the
	 * DCS/init layer (link analog, connector, panel).
	 */
	{
		uint8_t allon[1]  = { 0x23 };
		int trial;
		for (trial = 1; trial <= 4; trial++) {
			int inverted = (trial == 2) || (trial == 4);
			int lp       = (trial >= 3);
			uc_log("[fbd] matrix trial %d: reset=%s cmds=%s\n",
					trial, inverted ? "inverted" : "normal",
					lp ? "LP" : "HS");
			uc_backlight_blink((uint32_t)trial);
			uc_dsi_set_cmd_lp(lp);
			if (inverted) {
				uc_panel_reset_inverted();
			} else {
				uc_panel_reset();
			}
			uc_cwu50_init();
			uc_dsi_dcs_write(0x05, allon, 1);
			uc_mdelay(5000);
			/* no 0x13 restore: next trial's reset wipes state */
		}
		uc_dsi_set_cmd_lp(0);
		/* Back to the baseline config for the video attempt. */
		uc_panel_reset();
		uc_cwu50_init();
	}

	/*
	 * Full pre-video state snapshot into /fbd.log: measured clocks
	 * above plus every CPRMAN and DSI1 register of interest. Saved
	 * NOW so the log survives even if the video pipeline below
	 * wedges the machine.
	 */
	uc_clock_dump();
	uc_dsi_dump();
	uc_log_save();

	/* Prime the scan-out buffer BEFORE the pipeline starts fetching. */
	fill_red(&_fb_info);

	/*
	 * Mirror the DRM atomic commit path:
	 *   drm_atomic_bridge_chain_pre_enable  → vc4_dsi_bridge_pre_enable
	 *     (already done by uc_dsi_bringup above: DSI PHY up, DISP0 in
	 *      video mode with ENABLE bit clear.)
	 *   crtc->atomic_enable                 → vc4_crtc_atomic_enable
	 *     1. vc4_hvs_atomic_enable  (channel + dlist + AUTOHS)
	 *     2. vc4_crtc_config_pv     (all PV regs, EN=0, VIDEN=0)
	 *     3. PV_CONTROL |= EN
	 *     4. PV_V_CONTROL |= VIDEN
	 *   drm_atomic_bridge_chain_enable      → vc4_dsi_bridge_enable
	 *     5. DISP0_CTRL |= ENABLE   (LAST — after PV is pushing pixels)
	 */
	uc_hvs_bringup(_fb_info.phy_base, w, h, dep);
	uc_pv_configure();
	uc_pv_enable();
	uc_pv_video_enable();
	uc_dsi_video_mode();

	/*
	 * Runtime evidence, not just "we wrote the registers":
	 *  stage 7: channel 1 left INIT => the PV actually sent a vstart
	 *           and the HVS video engine is in RUN/EOF.
	 *  stage 8: the frame counter advances => PV keeps consuming
	 *           frames, i.e. DSI1 video mode is draining pixels.
	 * If 7 fails: PV never started (PV/DSI video-mode handshake).
	 * If 8 fails: one frame went out then stalled (DSI throughput /
	 * timing mismatch).
	 */
	uc_mdelay(100);
	if (uc_hvs_channel_running() != 0) {
		return fail_stage(7);
	}
	uc_log("[fbd] stage 7 OK: HVS ch1 video engine running\n");
	uc_backlight_blink(7);

	if (uc_hvs_frames_advancing(300) != 0) {
		return fail_stage(8);
	}
	uc_log("[fbd] stage 8 OK: frame counter advancing, video live\n");
	uc_backlight_blink(8);
	fill_red(&_fb_info);

	/*
	 * WHITE-SCREEN DISCRIMINATION (runs only after 7+8 pass).
	 * White = HVS background fill = the plane was dropped somewhere.
	 * The HVS re-parses the dlist every frame and rewrites the two
	 * context words; DISPSTAT latches AXI errors + underflow.  Blink:
	 *   after 8:  9 = plane SKIPPED (dlist rejected, ctx untouched)
	 *            10 = plane processed but AXI FETCH ERROR (address)
	 *            11 = channel underflow (fetch too slow)
	 *            12 = plane fetch HEALTHY -- the white is real
	 *                 content, red follows for 8s to prove it
	 * If NOT healthy (9/10/11), a PTR0 bus-alias A/B trial follows:
	 * blink N then 8s of screen time with the framebuffer address
	 * in flavour N:
	 *   1 = phy | 0xC0000000 (current)   2 = raw phy
	 *   3 = phy | 0x80000000
	 * Whichever trial shows RED names the correct alias.
	 */
	{
		int pr = uc_hvs_plane_probe();
		uc_log("[fbd] plane probe = %d\n", pr);
		if (pr == 0) {
			uc_backlight_blink(12);
			uc_mdelay(8000);   /* hold the red frame on screen */
		} else {
			uc_backlight_blink(pr == 1 ? 9 : (pr == 2 ? 10 : 11));
			uc_backlight_blink(1);
			uc_hvs_set_ptr0(_fb_info.phy_base | 0xC0000000U);
			uc_mdelay(8000);
			uc_backlight_blink(2);
			uc_hvs_set_ptr0(_fb_info.phy_base);
			uc_mdelay(8000);
			uc_backlight_blink(3);
			uc_hvs_set_ptr0(_fb_info.phy_base | 0x80000000U);
			uc_mdelay(8000);
			/* leave the upstream alias in place afterwards */
			uc_hvs_set_ptr0(_fb_info.phy_base | 0xC0000000U);
		}
	}
	uc_log_save();
	return 0;
}

int main(int argc, char** argv) {
	fbd_t fbd;
	const char* mnt_point = (argc > 1) ? argv[1] : "/dev/fb0";
	(void)_conf_file;

	fbd.splash = red_splash;
	fbd.flush = flush;
	fbd.init = init;
	fbd.get_info = get_info;

	return fbd_run(&fbd, mnt_point,
			UC_PANEL_WIDTH, UC_PANEL_HEIGHT, _conf_file);
}
