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

	/* Panel reset + backlight (GPIO-only, safe before any pixel path). */
	uc_time_init();
	uc_panel_reset();
	uc_backlight_init();
	uc_backlight_set(UC_BACKLIGHT_DEFAULT);

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

	if (uc_clock_bringup_dsi1(UC_DSI_HS_CLOCK_HZ) != 0) {
		return -1;
	}
	if (uc_dsi_bringup() != 0) {
		return -1;
	}

	if (map_scanout_buffer(w, h, dep) != 0) {
		return -1;
	}

	/*
	 * Panel DCS init must run BEFORE we let HVS/PV start pushing
	 * pixels: uc_dsi_bringup() left DISP0_CTRL programmed in video
	 * mode with ENABLE clear so the panel wakes up without racing
	 * against uninitialised pixel traffic.
	 */
	uc_cwu50_init();

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
	fill_red(&_fb_info);
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
