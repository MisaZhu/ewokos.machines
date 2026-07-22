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
#include "uc_hvs.h"
#include "uc_pv.h"
#include "uc_power.h"
#include "uc_pmu.h"

/*
 * ClockworkPi CM4 framebuffer daemon (uConsole 5" cwu50 and DevTerm
 * 6.86" cwd686 — same binary, panel picked at runtime from the
 * configured width, see uc_panel.h).
 *
 * This daemon drives the whole MIPI DSI path from cold: PLLD_DSI1
 * clocks -> DSI1 PHY + controller -> panel reset -> OCP8178 backlight
 * -> panel DCS init table -> DSI video mode.
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
	uc_clock_dump();
	uc_dsi_dump();
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
 * Prime the whole scan-out buffer to black so the first frames the
 * pipeline fetches are defined content instead of leftover DRAM.
 */
static void fill_black(const fbinfo_t* fbi) {
	if (fbi == NULL || fbi->pointer == 0) {
		return;
	}
	memset((void*)(uintptr_t)fbi->pointer, 0, fbi->size);
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
	const uc_panel_mode_t* mode;

	/*
	 * Panel selection: /etc/framebuffer.json's width (already parsed
	 * by libfbd) is the switch — 480 means the DevTerm cwd686, any
	 * other value (or 0) the uConsole cwu50.  The scan-out geometry
	 * must match the physical panel, so w/h are then forced to the
	 * selected mode regardless of what the json said.
	 */
	uc_panel_select(w);
	mode = uc_panel_mode();
	w = mode->width;
	h = mode->height;
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
	 * just log it and continue.
	 */
	uc_pmu_display_power();

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
	{
		int st = uc_power_domain_get(UC_PWR_DOMAIN_DSI1);
		if (st == 0) {
			if (uc_power_domain_set(UC_PWR_DOMAIN_DSI1, 1) != 0) {
				return fail_stage(1);
			}
			uc_mdelay(20);
		}
		st = uc_power_domain_get(UC_PWR_DOMAIN_VIDEO_SCALER);
		if (st == 0) {
			uc_power_domain_set(UC_PWR_DOMAIN_VIDEO_SCALER, 1);
			uc_mdelay(20);
		}
	}

	if (uc_clock_bringup_dsi1(mode->hs_clock_hz) != 0) {
		return fail_stage(2);
	}
	if (uc_dsi_alive() != 0) {
		/* ID register wrong: register bus/power issue, not timing. */
		return fail_stage(3);
	}

	/* PHY timing computation must use the same HS clock target. */
	uc_dsi_set_hs_clock(mode->hs_clock_hz);
	if (uc_dsi_bringup() != 0 || uc_dsi_lanes_stopped() != 0) {
		/* PHY refused to drive LP-11 on the data lanes. */
		return fail_stage(4);
	}
	if (map_scanout_buffer(w, h, dep) != 0) {
		return fail_stage(5);
	}

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
	if (mode->init_table() != 0) {
		/*
		 * Non-zero = number of DCS commands whose TXPKT1_DONE never
		 * fired: the panel cannot have been initialised.
		 */
		return fail_stage(6);
	}

	/* Prime the scan-out buffer BEFORE the pipeline starts fetching. */
	fill_black(&_fb_info);

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
	if (uc_hvs_frames_advancing(300) != 0) {
		return fail_stage(8);
	}
	return 0;
}

int main(int argc, char** argv) {
	fbd_t fbd;
	const char* mnt_point = (argc > 1) ? argv[1] : "/dev/fb0";
	(void)_conf_file;

	memset(&fbd, 0, sizeof(fbd));
	fbd.splash = NULL;   /* default logo splash from libfbd */
	fbd.flush = flush;
	/* flush is a plain blit into the scan-out buffer, so the libfbd
	 * generic direct-to-fb rotation applies: it rotates the client
	 * frame straight into fb memory (destination row-sequential for
	 * NC write-combining), skipping the intermediate rotate buffer
	 * and the extra full-frame copy. */
	fbd.flush_rotate = fbd_rotate_to;
	fbd.init = init;
	fbd.get_info = get_info;

	return fbd_run(&fbd, mnt_point,
			UC_PANEL_WIDTH, UC_PANEL_HEIGHT, _conf_file);
}
