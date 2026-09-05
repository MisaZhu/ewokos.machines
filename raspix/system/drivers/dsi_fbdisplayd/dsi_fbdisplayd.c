#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <sysinfo.h>
#include <displayd/displayd.h>
#include <graph/graph.h>
#include <arch/bcm283x/dsi1.h>
#include <arch/bcm283x/i2c.h>
#include <tinyjson/tinyjson.h>
#include <ewoksys/klog.h>

#include "panel_uc.h"

/*
 * Waveshare DSI LCD framebuffer daemon for Raspberry Pi 3 (BCM2837)
 * and Pi 4 (BCM2711).  Two panel families share the 0x45 I2C address
 * but speak completely different protocols; the daemon auto-detects
 * which one is connected (see panel_setup):
 *
 *   PANEL_WS   Waveshare's own MCU family (4inch 480x800 etc.,
 *              Linux panel-waveshare-dsi): I2C-only control, no
 *              DCS over DSI, no reset GPIO.
 *   PANEL_RPI7 official RPi 7" touchscreen protocol clones
 *              (Waveshare "5inch DSI LCD" 800x480 etc., Linux
 *              vc4-kms-dsi-7inch overlay): ATTINY power controller
 *              (rpi-panel-attiny-regulator) + TC358762 DSI->DPI
 *              bridge initialized over DSI LP generic writes.
 *
 * This daemon drives the whole MIPI DSI path from cold:
 *   CPRMAN clocks (PLLD_DSI0/1 + DSInE + DSInP)
 *   -> DSI PHY/host into LP-11 (port auto-selected: DSI1 on both
 *      Pi3 and Pi4/CM4 — the display connector wiring; DSI0 fallback)
 *   -> panel enable over I2C (BSC0 on GPIO44/45, panel at 0x45)
 *   -> HVS dlist scan-out (channel 0 for DSI0, 2 for gen4 DSI1,
 *      1 for gen5 DSI1) -> PixelValve -> DSI video mode
 *
 * The SoC generation is detected at runtime inside the arch library;
 * the same binary runs on both Pi3 (gen4) and Pi4 (gen5).
 *
 * PANEL_WS control, mirroring the Linux panel-waveshare-dsi driver:
 *   probe writes:  0xc0=1, 0xc2=1, 0xac=1
 *   enable:        0xad=1
 *   backlight:     0xab = 0xff - brightness, then 0xaa = 1
 * Mode from the kernel's ws_panel_4_0_mode: 480x800 @ 50 MHz pixel
 * clock, hfp/hsw/hbp = 150/100/150, vfp/vsw/vbp = 20/100/20, RGB888,
 * 2 lanes, non-continuous HS clock (VIDEO | VIDEO_HSE |
 * CLOCK_NON_CONTINUOUS).
 *
 * PANEL_RPI7 control, mirroring rpi-panel-attiny-regulator.c +
 * tc358762.c: ATTINY PORTA/B/C rail+reset walk and PWM backlight
 * over I2C, TC358762 bridge registers programmed with 6-byte DSI
 * generic long writes in LP mode.  Mode from the firmware modeline
 * (panel-raspberrypi-touchscreen.c): 800x480 @ 25.9794 MHz,
 * hfp/hsw/hbp = 1/2/46, vfp/vsw/vbp = 7/2/21, RGB888, 1 lane,
 * continuous HS clock (VIDEO | VIDEO_SYNC_PULSE | LPM | VIDEO_HSE).
 *
 * Scan-out targets a fixed reserved DRAM window — the uconsole
 * fbdisplayd model (UCONSOLE_FB_PHYS_BASE) with a per-board base:
 *   >1GB boards : 0x3c100000, the Pi4 reserved FB window accepted by
 *                 check_mem_map_arch() via PHY_LOW_RESV_BASE;
 *   <=1GB boards: top of ARM RAM minus 32MB, inside the kernel's
 *                 64MB low-resv window (allocable_phy_mem_top is
 *                 shrunk by PHY_LOW_RESV_SIZE, so the kernel never
 *                 allocates there; the firmware gpu_mem carve starts
 *                 above total_phy_mem_size, so the VC never cached
 *                 or wrote a byte of it).
 * The firmware-allocated fb (ALLOCATE_FB) proved unreliable as the
 * scan-out source on the 3A+ (CPU stores landed, HVS fetch never
 * observed them), so we no longer ask the firmware for a buffer.
 *
 * REAL HARDWARE: config.txt gpu_mem sizing is irrelevant to this
 * window; the low-resv carve exists on every <=1GB board.
 */

#define WS_PANEL_I2C_ADDR      0x45U
#define WS_PANEL_I2C_SDA_GPIO  44
#define WS_PANEL_I2C_SCL_GPIO  45
/* DIP switch position "I2C1": panel MCU on the 40-pin header bus. */
#define WS_PANEL_I2C1_SDA_GPIO 2
#define WS_PANEL_I2C1_SCL_GPIO 3

/*
 * On-panel MCU power controller registers and state bits (same map
 * dsi_touchd uses on this exact hardware): reg 0x94 holds the high
 * byte (touch rail bits), 0x95 the low byte.
 */
#define WS_MCU_REG_TP          0x94U
#define WS_MCU_REG_LCD         0x95U
#define WS_PWR_AVDD            (1u << 0)
#define WS_PWR_PANEL_RESET     (1u << 1)
#define WS_PWR_ENABLE          (1u << 2)
#define WS_PWR_IOVCC           (1u << 4)
#define WS_PWR_VCC             (1u << 8)
#define WS_PWR_TS_RESET        (1u << 9)

/*
 * PANEL_RPI7: ATTINY power controller registers and port bits
 * (rpi-panel-attiny-regulator.c).  Same 0x45 address as the WS MCU;
 * REG_ID reading 0xde (ver 1) or 0xc3 (ver 2) is the detection
 * signature.
 */
#define RPI7_REG_ID            0x80U
#define RPI7_REG_PORTA         0x81U
#define RPI7_REG_PORTB         0x82U
#define RPI7_REG_PORTC         0x83U
#define RPI7_REG_POWERON       0x85U
#define RPI7_REG_PWM           0x86U
#define RPI7_REG_ADDR_L        0x8cU
#define RPI7_REG_ADDR_H        0x8dU
#define RPI7_REG_WRITE_H       0x90U
#define RPI7_REG_WRITE_L       0x91U

#define RPI7_ID_V1             0xdeU
#define RPI7_ID_V2             0xc3U

#define RPI7_PA_LCD_LR         (1u << 2)
#define RPI7_PB_LCD_MAIN       (1u << 7)
#define RPI7_PC_LED_EN         (1u << 0)
#define RPI7_PC_RST_TP_N       (1u << 1)
#define RPI7_PC_RST_LCD_N      (1u << 2)
#define RPI7_PC_RST_BRIDGE_N   (1u << 3)

/*
 * TC358762 DSI->DPI bridge registers (tc358762.c), written as 6-byte
 * DSI generic long writes: {addr_lo, addr_hi, val LE32}.
 */
#define TC_PPI_STARTPPI        0x0104U
#define TC_PPI_LPTXTIMECNT     0x0114U
#define TC_PPI_D0S_ATMR        0x0144U
#define TC_PPI_D1S_ATMR        0x0148U
#define TC_PPI_D0S_CLRSIPOCOUNT 0x0164U
#define TC_PPI_D1S_CLRSIPOCOUNT 0x0168U
#define TC_DSI_STARTDSI        0x0204U
#define TC_DSI_LANEENABLE      0x0210U
#define TC_LCDCTRL             0x0420U
#define TC_LCD_HS_HBP          0x0424U
#define TC_LCD_HDISP_HFP       0x0428U
#define TC_LCD_VS_VBP          0x042cU
#define TC_LCD_VDISP_VFP       0x0430U
#define TC_SPICMR              0x0450U
#define TC_SYSCTRL             0x0464U

#define TC_LANEENABLE_CLEN     (1u << 0)
#define TC_LANEENABLE_L0EN     (1u << 1)
/* VSDELAY(1) | RGB888 | UNK6 | VTGEN, positive syncs — the constant
 * the old panel-raspberrypi-touchscreen.c hardcoded (0x00100150). */
#define TC_LCDCTRL_MAGIC       0x00100150U
#define TC_SYSCTRL_MAGIC       0x040fU
#define TC_LPX_PERIOD          3U

/*
 * Built-in defaults: the kernel's ws_panel_4_0_mode.  Any field can
 * be overridden from the display config when it carries
 * "output":"dsi" (same pattern as the raspi5 fbdisplayd DPI conf),
 * so other panels of the Waveshare DSI family (same MCU, different
 * timings) work without a rebuild.
 */
static bcm283x_dsi1_mode_t _panel_mode = {
	.width = 480,
	.height = 800,
	.hfp = 150, .hsw = 100, .hbp = 150,
	.vfp = 20,  .vsw = 100, .vbp = 20,
	.pixel_clock_hz = 50000000U,
	.lanes = 2,
	.continuous_clock = 0,
};

/* PANEL_RPI7 mode: the firmware modeline with HFP=1 (see
 * panel-raspberrypi-touchscreen.c rpi_touchscreen_modes). */
static const bcm283x_dsi1_mode_t _rpi7_mode = {
	.width = 800,
	.height = 480,
	.hfp = 1,  .hsw = 2, .hbp = 46,
	.vfp = 7,  .vsw = 2, .vbp = 21,
	.pixel_clock_hz = 25979400U,
	.lanes = 1,
	.continuous_clock = 1,
};

#define PANEL_WS      0
#define PANEL_RPI7    1
/*
 * ClockworkPi uConsole 5" (cwu50) and DevTerm 6.86" (cwd686).  Unlike the
 * two families above these are raw MIPI DSI panels whose DDIC is brought
 * up by a vendor DCS command sequence over the link, plus a GPIO reset
 * pulse, an OCP8178 1-wire backlight and AXP223 PMIC rails — none of
 * which an I2C-controlled panel has.  All of that lives in panel_uc.c;
 * the DCS transport and the clock/PHY bring-up are in panel_uc_dsi.c, and
 * HVS/PV stay on the shared arch library.
 *
 * cwu50 comes in two mutually exclusive hardware batches that need
 * different DDIC init tables but identical timings (see the PROVENANCE
 * note in panel_uc.h), hence the separate PANEL_CWU50OLD kind.
 */
#define PANEL_CWU50     2
#define PANEL_CWD686    3
#define PANEL_CWU50OLD  4

#define PANEL_IS_UC(k)  ((k) == PANEL_CWU50 || (k) == PANEL_CWD686 || \
                         (k) == PANEL_CWU50OLD)

/* PANEL_* -> the UC_PANEL_* family id panel_uc.c expects. */
static int _uc_family(int kind) {
	switch (kind) {
	case PANEL_CWD686:   return UC_PANEL_CWD686;
	case PANEL_CWU50OLD: return UC_PANEL_CWU50OLD;
	default:             return UC_PANEL_CWU50;
	}
}

static const char* _panel_kind_name(int kind) {
	switch (kind) {
	case PANEL_RPI7:     return "rpi7(attiny+tc358762)";
	case PANEL_CWU50:    return "cwu50(dcs+ocp8178+axp223)";
	case PANEL_CWU50OLD: return "cwu50_old(dcs+ocp8178+axp223)";
	case PANEL_CWD686:   return "cwd686(dcs+ocp8178+axp223)";
	default:             return "ws(mcu)";
	}
}

/* Resolved panel family: conf "panel" key or REG_ID probe. */
static int _panel_kind = PANEL_WS;
static int _panel_kind_conf = -1;   /* -1 = auto-detect */
static int _conf_has_mode = 0;      /* conf carried explicit timing */

/* OCP8178 level currently driven; the `bl` dev.cmd knob moves it. */
static uint8_t _uc_bl_level = UC_BACKLIGHT_DEFAULT;

/*
 * Software contrast, the `ct` dev.cmd knob, transcribed from
 * fbdisplay6d.  It is a plain centre-preserving LUT applied on the flush
 * path, so it stays inert at the default 100% and never affects the
 * ws/rpi7 families, which do not register uc_dev_cmd at all.
 */
#define UC_CONTRAST_MIN_PCT      20
#define UC_CONTRAST_MAX_PCT      300
#define UC_CONTRAST_DEFAULT_PCT  100
#define UC_CONTRAST_STEP_PCT     10

static uint32_t _contrast_pct = UC_CONTRAST_DEFAULT_PCT;
static uint8_t _contrast_lut[256];

static const char* _conf_file = "";
static int _display_index = 0;
static disp_info_t _fb_info;
/* Hoisted out of main() so contrast_sync_fast_paths() can reach it. */
static displayd_t _fbdisplayd_cfg;

static int doargs(int argc, char* argv[]) {
	int c = 0;

	while (c != -1) {
		c = getopt(argc, argv, "c:i:");
		if (c == -1)
			break;

		switch (c) {
		case 'c':
			_conf_file = optarg;
			break;
		case 'i':
			_display_index = atoi(optarg);
			break;
		default:
			c = -1;
			break;
		}
	}
	return optind;
}

static void load_panel_conf(const char* conf_file) {
	if (conf_file == NULL || conf_file[0] == '\0')
		conf_file = "/etc/display.json";

	json_var_t* conf_var = json_parse_file(conf_file);
	if (conf_var == NULL)
		return;

	if (strcmp(json_get_str_def(conf_var, "output", ""), "dsi") != 0) {
		json_var_unref(conf_var);
		return;
	}

	/* Optional panel-family override; "auto" (default) probes the
	 * MCU.  Forcing "rpi7" swaps in its mode defaults first so the
	 * timing keys below only need to list deviations.  The ClockworkPi
	 * families are never auto-detected: a raw DSI panel has no control
	 * MCU at 0x45 to probe, so the conf must name it. */
	{
		const char* panel = json_get_str_def(conf_var, "panel", "auto");
		int uc = uc_panel_from_name(panel);
		if (uc >= 0) {
			switch (uc) {
			case UC_PANEL_CWD686:   _panel_kind_conf = PANEL_CWD686;   break;
			case UC_PANEL_CWU50OLD: _panel_kind_conf = PANEL_CWU50OLD; break;
			default:                _panel_kind_conf = PANEL_CWU50;    break;
			}
			_panel_mode = *uc_panel_mode(uc);
		} else if (strcmp(panel, "ws") == 0) {
			_panel_kind_conf = PANEL_WS;
		} else if (strcmp(panel, "rpi7") == 0) {
			_panel_kind_conf = PANEL_RPI7;
			_panel_mode = _rpi7_mode;
		}
	}
	_conf_has_mode = 1;

	/*
	 * Geometry overrides are skipped for the ClockworkPi panels: their
	 * vendor DCS init table is a matched set with the native mode, so a
	 * conf width/height disagreeing with the panel would drive glass
	 * whose DDIC was never initialised for those timings.  The
	 * blanking / pixel-clock / lane keys stay honoured so a new panel
	 * of the same family can be tried without a rebuild.
	 */
	if (!PANEL_IS_UC(_panel_kind_conf)) {
		_panel_mode.width  = (uint32_t)json_get_int_def(conf_var, "width",  (int)_panel_mode.width);
		_panel_mode.height = (uint32_t)json_get_int_def(conf_var, "height", (int)_panel_mode.height);
	}
	_panel_mode.hfp    = (uint32_t)json_get_int_def(conf_var, "hfp",    (int)_panel_mode.hfp);
	_panel_mode.hsw    = (uint32_t)json_get_int_def(conf_var, "hsync",  (int)_panel_mode.hsw);
	_panel_mode.hbp    = (uint32_t)json_get_int_def(conf_var, "hbp",    (int)_panel_mode.hbp);
	_panel_mode.vfp    = (uint32_t)json_get_int_def(conf_var, "vfp",    (int)_panel_mode.vfp);
	_panel_mode.vsw    = (uint32_t)json_get_int_def(conf_var, "vsync",  (int)_panel_mode.vsw);
	_panel_mode.vbp    = (uint32_t)json_get_int_def(conf_var, "vbp",    (int)_panel_mode.vbp);
	_panel_mode.pixel_clock_hz = (uint32_t)json_get_int_def(conf_var, "pclk",
			(int)_panel_mode.pixel_clock_hz);
	_panel_mode.lanes  = (uint32_t)json_get_int_def(conf_var, "lanes",  (int)_panel_mode.lanes);
	_panel_mode.continuous_clock = json_get_int_def(conf_var, "cont_clock",
			_panel_mode.continuous_clock);
	json_var_unref(conf_var);
	slog("dsi_fbdisplayd: conf mode %ux%u pclk=%u lanes=%u\n",
			_panel_mode.width, _panel_mode.height,
			_panel_mode.pixel_clock_hz, _panel_mode.lanes);
}

static int contrast_active(void) {
	return _contrast_pct != UC_CONTRAST_DEFAULT_PCT;
}

/* Centre-preserving gain: 128 maps to 128 at any percentage. */
static void contrast_build_lut(void) {
	for (int32_t i = 0; i < 256; ++i) {
		int32_t v = (((i - 128) * (int32_t)_contrast_pct) / 100) + 128;

		if (v < 0)
			v = 0;
		else if (v > 255)
			v = 255;
		_contrast_lut[i] = (uint8_t)v;
	}
}

static inline uint32_t contrast_pixel(uint32_t s) {
	return (s & 0xff000000U) |
			((uint32_t)_contrast_lut[(s >> 16) & 0xff] << 16) |
			((uint32_t)_contrast_lut[(s >> 8) & 0xff] << 8) |
			(uint32_t)_contrast_lut[s & 0xff];
}

static uint16_t rgb565_from_u32(uint32_t s) {
	uint8_t r = (uint8_t)((s >> 16) & 0xff);
	uint8_t g = (uint8_t)((s >> 8) & 0xff);
	uint8_t b = (uint8_t)(s & 0xff);
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint32_t blt32_pitch(const disp_info_t* fbinfo, const graph_t* g) {
	uint32_t bytes_per_pixel = fbinfo->depth / 8;
	uint8_t* dst = (uint8_t*)(uintptr_t)(fbinfo->pointer +
			fbinfo->yoffset * fbinfo->pitch +
			fbinfo->xoffset * bytes_per_pixel);
	uint32_t total_bytes = (uint32_t)g->h * (uint32_t)g->w * bytes_per_pixel;

	/* wrap the framebuffer as a graph (row stride = pitch) and let
	   graph_blt run the arch-accelerated 1:1 copy */
	graph_t fb_g;
	graph_init(&fb_g, (const uint32_t*)dst,
			(int32_t)(fbinfo->pitch / bytes_per_pixel),
			(int32_t)fbinfo->yoffset + g->h);
	graph_blt((graph_t*)g, 0, 0, g->w, g->h,
			&fb_g, 0, 0, g->w, g->h);
	return total_bytes;
}

static uint32_t blt16_pitch(const disp_info_t* fbinfo, const graph_t* g) {
	uint8_t* dst_base = (uint8_t*)(uintptr_t)fbinfo->pointer +
			fbinfo->yoffset * fbinfo->pitch +
			fbinfo->xoffset * 2;
	int ct = contrast_active();

	for (int32_t y = 0; y < g->h; ++y) {
		uint16_t* dst_row = (uint16_t*)(dst_base + y * fbinfo->pitch);
		const uint32_t* src_row = g->buffer + y * g->w;

		for (int32_t x = 0; x < g->w; ++x) {
			uint32_t s = src_row[x];
			dst_row[x] = rgb565_from_u32(ct ? contrast_pixel(s) : s);
		}
	}
	return (uint32_t)g->w * (uint32_t)g->h * 2U;
}

static uint32_t flush(const disp_info_t* fbinfo, const graph_t* g) {
	const disp_info_t* phy = fbinfo != NULL ? fbinfo : &_fb_info;

	if (phy == NULL || phy->pointer == 0)
		return 0;
	if (phy->depth != 32 && phy->depth != 16)
		return 0;

	if (phy->depth == 16)
		return blt16_pitch(phy, g);

	/*
	 * Zero-copy fast path: rendering already targets the framebuffer.
	 * Contrast cannot be honoured here even when active — source and
	 * destination are the same pixels, so a LUT pass would re-apply
	 * itself on every frame and progressively destroy the image.
	 * blt32_pitch() stays contrast-free for the same reason, so the
	 * LUT is a 16bpp-only feature exactly as it was in fbdisplay6d.
	 */
	if ((uintptr_t)phy->pointer == (uintptr_t)g->buffer)
		return (uint32_t)g->w * (uint32_t)g->h * 4U;
	return blt32_pitch(phy, g);
}

static disp_info_t* get_info(void) {
	return &_fb_info;
}

/*
 * Fatal bring-up failure: report the stage so the wiring harness can
 * be diagnosed stage by stage.
 *   1 power domain   2 CPRMAN clocks     3 DSI1 ID probe
 *   4 PHY LP-11      5 scan-out mapping  6 panel I2C
 *   7 HVS not running 8 frames stalled
 */
static int32_t fail_stage(int stage) {
	printf("dsi_fbdisplayd: bring-up failed at stage %d\n", stage);
	/* The register banks + port probes tell the story. */
	if (stage >= 1) {
		bcm283x_dsi1_dump();
	}
	/*
	 * A handheld has no console to read, so also flag the stage on the
	 * backlight.  Finite, unlike the standalone uConsole daemon's
	 * uc_backlight_panic(): this returns and lets the daemon exit
	 * normally so init can carry on.
	 */
	if (PANEL_IS_UC(_panel_kind)) {
		uc_backlight_blink((uint32_t)stage);
	}
	return -1;
}

static uint32_t pick_fb_phys_base(void) {
	sys_info_t si;

	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&si);
	if (si.total_phy_mem_size > 0x40000000ULL)
		return 0x3c100000U;	/* Pi4 reserved FB window */
	return (uint32_t)si.total_phy_mem_size - 32U * 1024U * 1024U;
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
	_fb_info.phy_base = PANEL_IS_UC(_panel_kind) ?
			UC_PANEL_FB_PHYS_BASE : pick_fb_phys_base();
	_fb_info.size = size;
	_fb_info.size_max = size_max;
	_fb_info.xoffset = 0;
	_fb_info.yoffset = 0;
	_fb_info.dma_id = -1;

	/* Same vaddr choice as uconsole: just past the sys_dma window. */
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
static void fill_black(const disp_info_t* fbi) {
	if (fbi == NULL || fbi->pointer == 0)
		return;
	memset((void*)(uintptr_t)fbi->pointer, 0, fbi->size);
}

/*
 * The panel's I2C DIP switch selects which bus the control MCU
 * answers on: "I2C0" = BSC0 through the DSI FPC (GPIO44/45, Linux
 * i2c_csi_dsi — the shipping overlay default), "I2C1" = BSC1 on the
 * 40-pin header (GPIO2/3, the overlay's i2c1 parameter).  Probe the
 * FPC bus first and fall back to the header bus; once the MCU ACKs
 * the bus is latched, so re-inits skip the probe.  The BSC layer
 * drives one active controller at a time — every ws_panel_i2c_*
 * transfer below follows whichever bus this init selected.
 */
static int _ws_i2c_bus = -1;

static int32_t ws_panel_i2c_init(void) {
	if (_ws_i2c_bus != 1) {
		if (bcm283x_i2c0_init(WS_PANEL_I2C_SDA_GPIO,
				WS_PANEL_I2C_SCL_GPIO) == 0 &&
				(_ws_i2c_bus == 0 ||
				bcm283x_i2c0_probe(WS_PANEL_I2C_ADDR) == 0)) {
			_ws_i2c_bus = 0;
			return 0;
		}
	}
	if (bcm283x_i2c1_init(WS_PANEL_I2C1_SDA_GPIO,
			WS_PANEL_I2C1_SCL_GPIO) == 0 &&
			(_ws_i2c_bus == 1 ||
			bcm283x_i2c1_probe(WS_PANEL_I2C_ADDR) == 0)) {
		_ws_i2c_bus = 1;
		return 0;
	}
	_ws_i2c_bus = -1;
	printf("dsi_fbdisplayd: panel MCU 0x%02x on neither BSC0 (GPIO%d/%d) nor BSC1 (GPIO%d/%d)\n",
			WS_PANEL_I2C_ADDR,
			WS_PANEL_I2C_SDA_GPIO, WS_PANEL_I2C_SCL_GPIO,
			WS_PANEL_I2C1_SDA_GPIO, WS_PANEL_I2C1_SCL_GPIO);
	return -1;
}

/*
 * Waveshare panel control register write (smbus byte-data = the two
 * raw bytes {reg, val}).
 */
static int32_t ws_panel_i2c_write(uint8_t reg, uint8_t val) {
	uint8_t buf[2] = { reg, val };

	if (bcm283x_i2c0_write(WS_PANEL_I2C_ADDR, buf, 2) != 0) {
		printf("dsi_fbdisplayd: panel I2C write 0x%02x=0x%02x failed\n",
				reg, val);
		return -1;
	}
	return 0;
}

/*
 * Panel bring-up over I2C, mirroring panel-waveshare-dsi but split
 * the way DRM splits bridge pre_enable/enable (the shipping
 * vc4-kms-dsi-waveshare-panel overlay relies on that order):
 *   pre_enable: bus init + probe writes (0xc0/0xc2/0xac) while the
 *               host still sits at LP-11;
 *   enable:     display-on (0xad=1) + backlight (0xab/0xaa) ONLY once
 *               the DSI video stream is live.  Writing 0xad before a
 *               valid stream lets the bridge latch a no-signal state
 *               (black panel) that only clears on its slow retry.
 * The panel has no reset GPIO and no DCS path; the MCU's I2C bus
 * (selected by the DIP switch, see ws_panel_i2c_init) is the only
 * control channel.
 */
static int32_t ws_panel_pre_enable(void) {
	if (ws_panel_i2c_init() != 0)
		return -1;

	if (ws_panel_i2c_write(0xc0, 0x01) != 0 ||
			ws_panel_i2c_write(0xc2, 0x01) != 0 ||
			ws_panel_i2c_write(0xac, 0x01) != 0) {
		return -1;
	}
	return 0;
}

static int32_t ws_panel_enable(void) {
	if (ws_panel_i2c_write(0xad, 0x01) != 0 ||
			ws_panel_i2c_write(0xab, 0x00) != 0 ||	/* 0xff - 255 */
			ws_panel_i2c_write(0xaa, 0x01) != 0) {
		return -1;
	}
	return 0;
}

static int32_t ws_panel_i2c_read(uint8_t reg, uint8_t* val) {
	if (bcm283x_i2c0_write_read(WS_PANEL_I2C_ADDR, &reg, 1, val, 1) != 0)
		return -1;
	return 0;
}

static int32_t ws_mcu_set_power(uint16_t state, uint32_t settle_ms) {
	if (ws_panel_i2c_write((uint8_t)WS_MCU_REG_TP, (uint8_t)(state >> 8)) != 0)
		return -1;
	if (ws_panel_i2c_write((uint8_t)WS_MCU_REG_LCD, (uint8_t)(state & 0xff)) != 0)
		return -1;
	bcm283x_dsi1_mdelay(settle_ms);
	return 0;
}

static int32_t ws_mcu_get_power(uint16_t* state) {
	uint8_t tp = 0, lcd = 0;

	if (ws_panel_i2c_read(WS_MCU_REG_TP, &tp) != 0 ||
			ws_panel_i2c_read(WS_MCU_REG_LCD, &lcd) != 0)
		return -1;
	*state = (uint16_t)(((uint16_t)tp << 8) | lcd);
	return 0;
}

/*
 * Panel rails up BEFORE any DSI PHY activity.  The uConsole port
 * learned this the hard way (see its fbdisplayd): driving LP-11 into
 * an unpowered DDIC parasitically powers it through the ESD diodes,
 * so the real rail arrival never triggers a clean POR and the panel
 * latches deaf to reset and init.  The rail walk mirrors dsi_touchd,
 * which provably works on this hardware: VCC+ENABLE, then
 * AVDD+IOVCC.  Failure is logged, not fatal: the MCU may already
 * hold the rails on from its own defaults.
 */
static void ws_panel_power(void) {
	uint16_t state = 0;

	if (ws_panel_i2c_init() != 0)
		return;
	if (ws_mcu_get_power(&state) != 0) {
		printf("dsi_fbdisplayd: mcu power sync failed\n");
		return;
	}
	state |= WS_PWR_VCC | WS_PWR_ENABLE;
	if (ws_mcu_set_power(state, 20) != 0) {
		printf("dsi_fbdisplayd: mcu vcc/enable write failed\n");
		return;
	}
	state |= WS_PWR_AVDD | WS_PWR_IOVCC;
	if (ws_mcu_set_power(state, 60) != 0) {
		printf("dsi_fbdisplayd: mcu avdd/iovcc write failed\n");
		return;
	}
}

/*
 * Reset pulse AFTER the DSI host parks the lanes in LP-11, matching
 * DRM's host pre_enable -> panel prepare ordering and dsi_touchd's
 * pulse shape (assert 20ms, release 180ms): the DDIC comes out of
 * reset seeing LP-11 on the lanes.
 */
static int32_t ws_panel_reset_pulse(void) {
	uint16_t state = 0;

	if (ws_mcu_get_power(&state) != 0)
		return -1;
	if (ws_mcu_set_power(state | WS_PWR_PANEL_RESET, 20) != 0)
		return -1;
	if (ws_mcu_set_power(state & (uint16_t)~WS_PWR_PANEL_RESET, 180) != 0)
		return -1;
	return 0;
}

/* ---- PANEL_RPI7: ATTINY power controller + TC358762 bridge ---- */

static int32_t rpi7_bridge_write(uint16_t reg, uint32_t val) {
	uint8_t buf[6] = {
		(uint8_t)(reg & 0xffU), (uint8_t)(reg >> 8),
		(uint8_t)(val & 0xffU), (uint8_t)((val >> 8) & 0xffU),
		(uint8_t)((val >> 16) & 0xffU), (uint8_t)((val >> 24) & 0xffU),
	};

	/* 0x29 = generic long write, in LP mode (MIPI_DSI_MODE_LPM). */
	if (bcm283x_dsi1_cmd_write(0x29, buf, sizeof(buf), 1) != 0) {
		printf("dsi_fbdisplayd: TC358762 write 0x%04x=0x%08x failed\n",
				reg, val);
		return -1;
	}
	return 0;
}

/*
 * ATTINY rails up BEFORE any DSI PHY activity (same doctrine as
 * ws_panel_power).  Mirrors attiny_i2c_probe() +
 * attiny_lcd_power_enable(): everything held in reset, orientation,
 * LCD rails on, LED enable — the bridge stays in reset until
 * rpi7_bridge_enable() (its reset is the "vddc regulator" in Linux).
 */
static void rpi7_panel_power(void) {
	if (ws_panel_i2c_init() != 0)
		return;
	ws_panel_i2c_write(RPI7_REG_POWERON, 0x00);
	bcm283x_dsi1_mdelay(30);
	ws_panel_i2c_write(RPI7_REG_PWM, 0x00);
	bcm283x_dsi1_mdelay(10);

	ws_panel_i2c_write(RPI7_REG_PORTC, 0x00);
	bcm283x_dsi1_mdelay(10);
	ws_panel_i2c_write(RPI7_REG_PORTA, RPI7_PA_LCD_LR);
	bcm283x_dsi1_mdelay(10);
	ws_panel_i2c_write(RPI7_REG_PORTB, RPI7_PB_LCD_MAIN);
	bcm283x_dsi1_mdelay(10);
	ws_panel_i2c_write(RPI7_REG_PORTC, RPI7_PC_LED_EN);
	bcm283x_dsi1_mdelay(80);
}

/*
 * Bridge out of reset + TC358762 init, with the host at LP-11 — the
 * DRM pre_enable analog (tc358762_pre_enable with
 * pre_enable_prev_first, so the DSI host is already up).  The PORTC
 * write is Linux's "vddc" fixed regulator (RST_BRIDGE_N gpio) plus
 * the attiny_gpio_set() firmware magic that follows it; TP reset is
 * released too so a future touch daemon finds the controller alive.
 * The register recipe is tc358762_init() verbatim.
 */
static int32_t rpi7_bridge_enable(void) {
	int32_t rc = 0;

	if (ws_panel_i2c_write(RPI7_REG_PORTC,
			RPI7_PC_LED_EN | RPI7_PC_RST_TP_N |
			RPI7_PC_RST_LCD_N | RPI7_PC_RST_BRIDGE_N) != 0)
		return -1;
	bcm283x_dsi1_mdelay(8);
	ws_panel_i2c_write(RPI7_REG_ADDR_H, 0x04);
	bcm283x_dsi1_mdelay(8);
	ws_panel_i2c_write(RPI7_REG_ADDR_L, 0x7c);
	bcm283x_dsi1_mdelay(8);
	ws_panel_i2c_write(RPI7_REG_WRITE_H, 0x00);
	bcm283x_dsi1_mdelay(8);
	ws_panel_i2c_write(RPI7_REG_WRITE_L, 0x00);
	bcm283x_dsi1_mdelay(100);

	rc |= rpi7_bridge_write(TC_DSI_LANEENABLE,
			TC_LANEENABLE_CLEN | TC_LANEENABLE_L0EN);
	rc |= rpi7_bridge_write(TC_PPI_D0S_CLRSIPOCOUNT, 5);
	rc |= rpi7_bridge_write(TC_PPI_D1S_CLRSIPOCOUNT, 5);
	rc |= rpi7_bridge_write(TC_PPI_D0S_ATMR, 0);
	rc |= rpi7_bridge_write(TC_PPI_D1S_ATMR, 0);
	rc |= rpi7_bridge_write(TC_PPI_LPTXTIMECNT, TC_LPX_PERIOD);
	rc |= rpi7_bridge_write(TC_SPICMR, 0x00);
	rc |= rpi7_bridge_write(TC_LCDCTRL, TC_LCDCTRL_MAGIC);
	rc |= rpi7_bridge_write(TC_SYSCTRL, TC_SYSCTRL_MAGIC);
	rc |= rpi7_bridge_write(TC_LCD_HS_HBP,
			_panel_mode.hsw | (_panel_mode.hbp << 16));
	rc |= rpi7_bridge_write(TC_LCD_HDISP_HFP,
			_panel_mode.width | (_panel_mode.hfp << 16));
	rc |= rpi7_bridge_write(TC_LCD_VS_VBP,
			_panel_mode.vsw | (_panel_mode.vbp << 16));
	rc |= rpi7_bridge_write(TC_LCD_VDISP_VFP,
			_panel_mode.height | (_panel_mode.vfp << 16));
	bcm283x_dsi1_mdelay(100);

	rc |= rpi7_bridge_write(TC_PPI_STARTPPI, 1);
	rc |= rpi7_bridge_write(TC_DSI_STARTDSI, 1);
	bcm283x_dsi1_mdelay(100);

	return rc != 0 ? -1 : 0;
}

static int32_t rpi7_backlight(void) {
	return ws_panel_i2c_write(RPI7_REG_PWM, 0xff);
}

/*
 * dev.cmd backlight knob for the ClockworkPi panels.  The OCP8178 sits
 * on its own 1-wire GPIO, so a level change never touches the scan-out
 * path and needs no repaint.  The contrast knob below is the opposite
 * case: it rewrites pixels, so it must force one.
 */
static void _uc_bl_set(int level) {
	if (level < 0)
		level = 0;
	if (level > UC_BACKLIGHT_MAX_LEVEL)
		level = UC_BACKLIGHT_MAX_LEVEL;
	_uc_bl_level = (uint8_t)level;
	uc_backlight_set(_uc_bl_level);
}

/*
 * The rotate and dirty-rect fast paths blit straight into the scan-out
 * buffer without passing through flush(), so neither can carry the
 * contrast LUT.  Pull them out while contrast is active and put them
 * back once it returns to 100%.
 */
static void contrast_sync_fast_paths(void) {
	if (contrast_active()) {
		_fbdisplayd_cfg.flush_rotate = NULL;
		fbdisplayd_set_flush_rect(NULL);
	} else {
		_fbdisplayd_cfg.flush_rotate = fbdisplayd_rotate_to;
		fbdisplayd_set_flush_rect(fbdisplayd_flush_rect_to);
	}
}

static uint32_t clamp_contrast_pct(int pct) {
	if (pct < UC_CONTRAST_MIN_PCT)
		return UC_CONTRAST_MIN_PCT;
	if (pct > UC_CONTRAST_MAX_PCT)
		return UC_CONTRAST_MAX_PCT;
	return (uint32_t)pct;
}

static void apply_contrast_pct(int pct) {
	_contrast_pct = clamp_contrast_pct(pct);
	contrast_build_lut();
	contrast_sync_fast_paths();
	/* Repaint what is already on screen through the new LUT. */
	fbdisplayd_refresh();
}

static char* uc_dev_cmd(int from_pid, int argc, char** argv) {
	char* ret = (char*)malloc(128);
	char* end = NULL;
	long requested = 0;

	(void)from_pid;
	if (ret == NULL)
		return NULL;
	if (argc <= 0 || argv == NULL || argv[0] == NULL) {
		free(ret);
		return NULL;
	}
	if (strcmp(argv[0], "help") == 0) {
		snprintf(ret, 128,
			"help: show commands\n"
			"bl [up|down|0-%d]: backlight level\n"
			"ct [up|down|%d-%d]: contrast percent (%d = off)\n",
			UC_BACKLIGHT_MAX_LEVEL,
			UC_CONTRAST_MIN_PCT, UC_CONTRAST_MAX_PCT,
			UC_CONTRAST_DEFAULT_PCT);
		return ret;
	}
	if (strcmp(argv[0], "bl") == 0) {
		if (argc < 2 || argv[1] == NULL) {
			snprintf(ret, 128, "backlight=%u/%d\n",
				(unsigned)_uc_bl_level, UC_BACKLIGHT_MAX_LEVEL);
			return ret;
		}
		if (strcmp(argv[1], "up") == 0) {
			_uc_bl_set((int)_uc_bl_level + 1);
		} else if (strcmp(argv[1], "down") == 0) {
			_uc_bl_set((int)_uc_bl_level - 1);
		} else {
			requested = strtol(argv[1], &end, 10);
			if (argv[1][0] == 0 || end == NULL || *end != 0) {
				snprintf(ret, 128, "usage: bl [up|down|0-%d]\n",
					UC_BACKLIGHT_MAX_LEVEL);
				return ret;
			}
			_uc_bl_set((int)requested);
		}
		snprintf(ret, 128, "backlight=%u/%d\n",
			(unsigned)_uc_bl_level, UC_BACKLIGHT_MAX_LEVEL);
		return ret;
	}
	if (strcmp(argv[0], "ct") == 0) {
		if (argc < 2 || argv[1] == NULL) {
			snprintf(ret, 128, "contrast=%u%%\n", (unsigned)_contrast_pct);
			return ret;
		}
		if (strcmp(argv[1], "up") == 0) {
			apply_contrast_pct((int)_contrast_pct + UC_CONTRAST_STEP_PCT);
		} else if (strcmp(argv[1], "down") == 0) {
			apply_contrast_pct((int)_contrast_pct - UC_CONTRAST_STEP_PCT);
		} else {
			requested = strtol(argv[1], &end, 10);
			if (argv[1][0] == 0 || end == NULL || *end != 0) {
				snprintf(ret, 128, "usage: ct [up|down|%d-%d]\n",
					UC_CONTRAST_MIN_PCT, UC_CONTRAST_MAX_PCT);
				return ret;
			}
			apply_contrast_pct((int)requested);
		}
		snprintf(ret, 128, "contrast=%u%%\n", (unsigned)_contrast_pct);
		return ret;
	}
	snprintf(ret, 128, "unknown command: %s\ntry: help\n", argv[0]);
	return ret;
}

/*
 * Resolve which panel family sits on the connector.  The conf
 * "panel" key wins; otherwise probe REG_ID (0x80) — only the ATTINY
 * answers 0xde/0xc3 there, the WS MCU does not.  On PANEL_RPI7 the
 * mode defaults are swapped in unless the conf carried explicit
 * timing.  Needs mmio (I2C) up, so this runs inside init().
 */
static void panel_setup(void) {
	if (_panel_kind_conf >= 0) {
		_panel_kind = _panel_kind_conf;
	} else {
		uint8_t id = 0;

		_panel_kind = PANEL_WS;
		if (ws_panel_i2c_init() == 0 &&
				ws_panel_i2c_read(RPI7_REG_ID, &id) == 0 &&
				(id == RPI7_ID_V1 || id == RPI7_ID_V2)) {
			_panel_kind = PANEL_RPI7;
			if (!_conf_has_mode)
				_panel_mode = _rpi7_mode;
		}
	}
	slog("dsi_fbdisplayd: panel %s %ux%u\n",
			_panel_kind_name(_panel_kind),
			_panel_mode.width, _panel_mode.height);
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
	bcm283x_dsi1_adjusted_mode_t adj;
	int st;

	if (dep != 16 && dep != 32)
		dep = 32;

	mmio_map();
	if (_mmio_base == 0)
		return -1;

	/*
	 * The firmware is still driving the panel: capture its exact
	 * DSI/PV/clock recipe before we overwrite any of it.
	 */
	bcm283x_dsi1_dump_firmware();

	/*
	 * Resolve the panel family (needs I2C, so after mmio_map), then
	 * force the scan-out geometry to the physical panel regardless
	 * of what the config said.
	 */
	panel_setup();
	w = _panel_mode.width;
	h = _panel_mode.height;

	/*
	 * Panel rails FIRST, before any DSI register/PHY activity
	 * (see ws_panel_power): a cold DDIC must see its rails up
	 * before the lanes go LP-11.
	 */
	if (PANEL_IS_UC(_panel_kind))
		uc_panel_prepare();
	else if (_panel_kind == PANEL_RPI7)
		rpi7_panel_power();
	else
		ws_panel_power();

	/*
	 * Firmware power domains: upstream Linux never power-cycles
	 * the DSI domains (no bcm27xx dtsi carries power-domains on
	 * &dsi0/&dsi1).  The only transition uconsole provably uses on
	 * real firmware is the VIDEO_SCALER (HVS) one, so enable just
	 * that when reported OFF; the port probe below is the real
	 * arbiter for everything else.
	 */
	if (PANEL_IS_UC(_panel_kind)) {
		/*
		 * fbdisplay6d's CM4 order: DSI1 domain first — and a failed
		 * set IS fatal there, since the glass hangs off that port and
		 * nothing downstream can rescue it — then VIDEO_SCALER.
		 * DSI0 is deliberately left alone: the ClockworkPi panels are
		 * DSI1-only, so powering a second analog island is a deviation
		 * from the proven sequence with nothing to gain.
		 */
		st = bcm283x_dsi1_power_domain_get(BCM283X_PWR_DOMAIN_DSI1);
		if (st == 0) {
			if (bcm283x_dsi1_power_domain_set(BCM283X_PWR_DOMAIN_DSI1, 1) != 0)
				return fail_stage(1);
			bcm283x_dsi1_mdelay(20);
		}
		st = bcm283x_dsi1_power_domain_get(BCM283X_PWR_DOMAIN_VIDEO_SCALER);
		if (st == 0) {
			bcm283x_dsi1_power_domain_set(BCM283X_PWR_DOMAIN_VIDEO_SCALER, 1);
			bcm283x_dsi1_mdelay(20);
		}
	} else {
		st = bcm283x_dsi1_power_domain_get(BCM283X_PWR_DOMAIN_VIDEO_SCALER);
		if (st == 0) {
			if (bcm283x_dsi1_power_domain_set(BCM283X_PWR_DOMAIN_VIDEO_SCALER, 1) != 0)
				return fail_stage(1);
			bcm283x_dsi1_mdelay(20);
		}

		/*
		 * The DSI analog AFE island is parked off after boot; a live
		 * digital bank with a dead AFE shows up as LP contention and
		 * lanes that never settle to STOP (exactly the cold-DSI0
		 * signature).  Firmware domain ids come from
		 * dt-bindings/power/raspberrypi-power.h (+1): DSI0=18, DSI1=19.
		 * There is no pixelvalve domain in the firmware table.  A
		 * failed set is not fatal (old firmware blobs may not know a
		 * domain id).
		 */
		{
			int d0 = bcm283x_dsi1_power_domain_get(BCM283X_PWR_DOMAIN_DSI0);
			int d1 = bcm283x_dsi1_power_domain_get(BCM283X_PWR_DOMAIN_DSI1);

			if (d0 == 0)
				bcm283x_dsi1_power_domain_set(BCM283X_PWR_DOMAIN_DSI0, 1);
			if (d1 == 0)
				bcm283x_dsi1_power_domain_set(BCM283X_PWR_DOMAIN_DSI1, 1);
			bcm283x_dsi1_mdelay(20);
		}
	}

	/*
	 * Pick the DSI port the panel actually hangs off: the display
	 * connector is wired to DSI1 on every supported board (Pi 3A+
	 * included — DSI0 pads are only bonded out on Compute Modules;
	 * on gen4 the DSI1 probe write must go through the DMA engine
	 * because of the broken AXI slave, probe_port handles that).
	 * Probe DSI1 first; DSI0 is the fallback for CM-only wiring.
	 * A powered-off port silently drops writes, so the first live
	 * port in that order wins.
	 */
	if (PANEL_IS_UC(_panel_kind)) {
		/*
		 * fbdisplay6d hardwires DSI1 and never probes.  Probing is
		 * not neutral here: if DSI1's write-readback missed and DSI0
		 * answered, every register write from then on would go to the
		 * wrong host — the backlight (plain GPIO) still comes up while
		 * the panel stays deaf, i.e. exactly "backlight on, no image".
		 */
		bcm283x_dsi1_set_port(1);
	} else {
		int first = 1;
		if (bcm283x_dsi1_probe_port(first) == 0) {
			bcm283x_dsi1_set_port(first);
		} else if (bcm283x_dsi1_probe_port(first ^ 1) == 0) {
			bcm283x_dsi1_set_port(first ^ 1);
		} else {
			/* Neither DSI block accepts writes. */
			printf("dsi_fbdisplayd: probe DSI0=%d DSI1=%d\n",
					bcm283x_dsi1_probe_port(0) == 0,
					bcm283x_dsi1_probe_port(1) == 0);
			return fail_stage(1);
		}
	}

	/*
	 * The firmware is still streaming video on this port; kill its
	 * DISP0/CTRL/PV before reprogramming the PHY, or the live HS
	 * transmitter fights ours (LP contention, lanes never stop).
	 * The ClockworkPi boards skip this: their VC firmware has no
	 * cwu50/cwd686 panel config at all, so there is no stream to hand
	 * off, and gating DSI CTRL is a step fbdisplay6d never performs.
	 */
	if (!PANEL_IS_UC(_panel_kind))
		bcm283x_dsi1_firmware_handoff();

	if (PANEL_IS_UC(_panel_kind)) {
		/*
		 * fbdisplay6d's clock and host layers, transcribed into
		 * panel_uc_dsi.c.  The shared library's equivalents are not
		 * used for these two stages: they deviate from the proven
		 * sequence in the PLLD VCO derivation, the DSI1E escape-clock
		 * parent and the CTRL_EN ordering, and every one of those
		 * lands inside stage 2 -> stage 4.  The full register-level
		 * list is in that file's header comment.
		 *
		 * The target is the vendor's hardcoded hs_clock_hz, and the
		 * HS clock PLLD actually generates (its return value) is what
		 * the PHY timing bank is built from — fbdisplay6d does exactly
		 * this by passing mode->hs_clock_hz to both uc_clock_bringup
		 * and uc_dsi_set_hs_clock.
		 */
		uint32_t hs = uc_clock_bringup_dsi1(
				uc_panel_hs_clock(_uc_family(_panel_kind)));
		if (hs == 0)
			return fail_stage(2);
		if (uc_dsi_alive() != 0) {
			/* ID register wrong: register bus/power issue, not timing. */
			return fail_stage(3);
		}
		if (uc_dsi_bringup(hs) != 0 || uc_dsi_lanes_stopped() != 0)
			/* PHY refused to drive LP-11 on the data lanes. */
			return fail_stage(4);

		/*
		 * HVS and PV stay on the shared library, so adj has to
		 * describe the mode uc_pv_configure() would have programmed:
		 * the RAW panel blanking (uc_pv.c writes mode->hfp verbatim —
		 * there is no integer-PLL compensation step in the uConsole
		 * tree), the HS clock PLLD really made, and
		 * PIX_CLK_DIV = 24bpp / lanes.
		 */
		adj.width  = _panel_mode.width;
		adj.height = _panel_mode.height;
		adj.hfp = _panel_mode.hfp;
		adj.hsw = _panel_mode.hsw;
		adj.hbp = _panel_mode.hbp;
		adj.vfp = _panel_mode.vfp;
		adj.vsw = _panel_mode.vsw;
		adj.vbp = _panel_mode.vbp;
		adj.hs_clock_hz = hs;
		adj.pix_clk_divider = (_panel_mode.lanes != 0) ?
				(24U / _panel_mode.lanes) : 6U;
		adj.pixel_clock_hz = hs / adj.pix_clk_divider;
	} else {
		if (bcm283x_dsi1_clock_bringup(&_panel_mode, &adj) != 0)
			return fail_stage(2);
		if (bcm283x_dsi1_alive() != 0) {
			/* ID register wrong: register bus/power issue, not timing. */
			return fail_stage(3);
		}

		if (bcm283x_dsi1_host_bringup(_panel_mode.lanes, adj.hs_clock_hz,
				_panel_mode.continuous_clock) != 0)
			return fail_stage(4);
		if (bcm283x_dsi1_lanes_stopped() != 0) {
			/*
			 * DSI1 STAT lane-stop bits are documented (and proven on
			 * CM4); DSI0's STAT layout is not, so on port 0 a miss is
			 * a warning, not a failure — upstream never checks.
			 */
			if (bcm283x_dsi1_port() == 0) {
				printf("dsi_fbdisplayd: WARN lanes not stopped\n");
				bcm283x_dsi1_dump();
			} else
				return fail_stage(4);
		}
	}

	/*
	 * Lanes are at LP-11.  PANEL_WS: DDIC reset pulse so it comes
	 * out of reset into a live bus.  PANEL_RPI7: bridge out of
	 * reset + the full TC358762 register init over DSI LP writes
	 * (display-on/backlight still waits for the live stream).
	 */
	/*
	 * fbdisplay6d maps the scan-out window BEFORE the panel init (its
	 * stage 5 precedes stage 6).  Keep that order for the uc families so
	 * the SYS_MEM_MAP syscall is not interleaved between the DCS table
	 * and the pipeline start.
	 */
	if (PANEL_IS_UC(_panel_kind) && map_scanout_buffer(w, h, dep) != 0)
		return fail_stage(5);

	if (PANEL_IS_UC(_panel_kind)) {
		/*
		 * Reset pulse then the vendor DCS table, both at LP-11 and
		 * both before HVS/PV start pushing pixels — upstream keeps
		 * DISP0_CTRL at 0 through panel init, and panel-*.c's
		 * .prepare() (which ends in SLPOUT + DSPON) runs at bridge
		 * pre_enable, not after the stream is live.
		 */
		uc_panel_reset_pulse();
		if (uc_panel_init_table(_uc_family(_panel_kind)) != 0)
			return fail_stage(6);
	} else if (_panel_kind == PANEL_RPI7) {
		if (rpi7_bridge_enable() != 0)
			return fail_stage(6);
	} else if (ws_panel_reset_pulse() != 0)
		printf("dsi_fbdisplayd: WARN panel reset pulse failed\n");

	if (!PANEL_IS_UC(_panel_kind) && map_scanout_buffer(w, h, dep) != 0)
		return fail_stage(5);

	/*
	 * DRM bridge pre_enable while the host sits at LP-11: the
	 * panel is probed/configured but NOT displayed yet.  The
	 * display-on + backlight writes wait for a live video stream
	 * (after stage 8) — enabling the bridge against a dead stream
	 * latches a no-signal black state.  PANEL_RPI7 has no I2C
	 * pre_enable step; its bridge init already ran above.
	 */
	if (_panel_kind == PANEL_WS && ws_panel_pre_enable() != 0)
		return fail_stage(6);

	/* Prime the scan-out buffer BEFORE the pipeline starts fetching. */
	fill_black(&_fb_info);

	/*
	 * Mirror the DRM atomic commit path (vc4_crtc_atomic_enable):
	 *   vc4_hvs_atomic_enable   (channel + dlist)
	 *   vc4_crtc_config_pv      (all PV regs, EN=0, VIDEN=0)
	 *   PV_CONTROL |= EN
	 *   vc4_dsi_encoder_enable  (DISP0_CTRL |= ENABLE, HS start)
	 *   PV_V_CONTROL |= VIDEN   (LAST — after the host can hand
	 *                            the PV its hstart handshake)
	 */
	if (bcm283x_dsi1_hvs_bringup(_fb_info.phy_base, w, h, dep,
			_fb_info.pitch) != 0)
		return fail_stage(7);
	bcm283x_dsi1_pv_configure(&adj);
	bcm283x_dsi1_pv_enable();
	if (PANEL_IS_UC(_panel_kind)) {
		/*
		 * fbdisplayd AND fbdisplay6d both kick PV VIDEN before the
		 * DISP0 video-mode write — the reverse of the
		 * vc4_crtc_atomic_enable order this library documents and the
		 * ws/rpi7 families use.  Both uConsole daemons are proven on
		 * this glass, so their order wins here.  The video-mode write
		 * itself goes through panel_uc_dsi.c too, since the host was
		 * brought up there and DISP0_CTRL has been 0 all along.
		 */
		bcm283x_dsi1_pv_video_enable();
		uc_dsi_video_mode();
	} else {
		bcm283x_dsi1_video_mode(adj.pix_clk_divider);
		bcm283x_dsi1_pv_video_enable();
	}

	/*
	 * Runtime evidence, not just "we wrote the registers":
	 *  stage 7: channel left INIT => the PV actually sent a vstart
	 *           and the HVS video engine is in RUN/EOF.
	 *  stage 8: the frame counter advances => PV keeps consuming
	 *           frames, i.e. DSI1 video mode is draining pixels.
	 * The budget is generous (400ms) because a slow first frame
	 * (PHY/video engine startup) must not read as a stall.  If the
	 * hstart handshake (WAIT_HSTART) is the blocker, clearing it
	 * lets the PV free-run: log which path got us running.
	 */
	if (PANEL_IS_UC(_panel_kind)) {
		/*
		 * fbdisplay6d's budgets (100ms/300ms) and no recovery path:
		 * clearing WAIT_HSTART or re-targeting the HVS crossbar both
		 * rewrite PV/HVS state the proven uConsole sequence never
		 * touches, so a miss is reported rather than papered over.
		 */
		if (bcm283x_dsi1_hvs_wait_running(100) != 0)
			return fail_stage(7);
		if (bcm283x_dsi1_hvs_frames_advancing(300) != 0)
			return fail_stage(8);
	} else {
		if (bcm283x_dsi1_hvs_wait_running(400) != 0) {
			printf("dsi: no vstart with WAIT_HSTART; retrying free-run\n");
			bcm283x_dsi1_pv_clear_wait_hstart();
			if (bcm283x_dsi1_hvs_wait_running(400) != 0) {
				/*
				 * PV may be scanning while its vstart lands on
				 * another FIFO than upstream's hardwired map
				 * says.  Probe the crossbar and follow it.
				 */
				if (bcm283x_dsi1_hvs_crossbar_probe() != 0)
					return fail_stage(7);
			}
		}
		if (bcm283x_dsi1_hvs_frames_advancing(500) != 0)
			return fail_stage(8);
	}

	/*
	 * Stream is provably live: DRM bridge enable step.  The panel
	 * now sees display-on (PANEL_WS) or backlight PWM (PANEL_RPI7)
	 * against a valid video stream and locks immediately instead
	 * of latching no-signal.
	 */
	if (PANEL_IS_UC(_panel_kind)) {
		/* SLPOUT/DSPON already ran at the tail of the DCS table and
		 * the backlight was lit in uc_panel_prepare(). */
	} else if (_panel_kind == PANEL_RPI7) {
		if (rpi7_backlight() != 0)
			return fail_stage(6);
	} else if (ws_panel_enable() != 0)
		return fail_stage(6);

	return 0;
}

int main(int argc, char** argv) {
	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti] : "/dev/disp0";

	load_panel_conf(_conf_file);
	if (PANEL_IS_UC(_panel_kind_conf))
		fbdisplayd_set_dev_cmd(uc_dev_cmd);

	memset(&_fbdisplayd_cfg, 0, sizeof(_fbdisplayd_cfg));
	_fbdisplayd_cfg.splash = NULL;   /* default logo splash from libdisplayd */
	_fbdisplayd_cfg.flush = flush;
	_fbdisplayd_cfg.init = init;
	_fbdisplayd_cfg.get_info = get_info;
	/*
	 * flush is a plain blit into the scan-out buffer, so libdisplayd's
	 * generic direct-to-fb rotation and dirty-rect push both apply —
	 * but only while contrast is off, since neither fast path goes
	 * through flush() where the LUT lives.  contrast_sync_fast_paths()
	 * installs them here and swaps them out on every `ct` change.
	 */
	contrast_build_lut();
	contrast_sync_fast_paths();

	return fbdisplayd_run(&_fbdisplayd_cfg, mnt_point,
			_panel_mode.width, _panel_mode.height,
			_conf_file, _display_index);
}
