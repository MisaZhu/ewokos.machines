#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include <ewoksys/syscall.h>
#include <ewoksys/dma.h>
#include <sysinfo.h>
#include <displayd/displayd.h>
#include <graph/graph.h>
#include <arch/bcm2712/rp1_dsi.h>
#include <arch/bcm2712/i2c.h>
#include <tinyjson/tinyjson.h>
#include <ewoksys/klog.h>

#include "panel_uc.h"

/*
 * Waveshare DSI LCD framebuffer daemon for Raspberry Pi 5 (BCM2712).
 *
 * Port of the raspix dsi_fbdisplayd: both panel families that answer
 * at control I2C 0x45 are supported and auto-detected (panel_setup,
 * overridable with the display config "panel" key):
 *
 *   PANEL_WS   Waveshare's own MCU family (4inch 480x800 etc., Linux
 *              panel-waveshare-dsi): I2C-only control, no DCS over
 *              DSI, no reset GPIO. 2 lanes, sync events, HS clock
 *              returns to LP in blanking.
 *   PANEL_RPI7 official RPi 7" touchscreen protocol clones
 *              (Waveshare 5inch DSI LCD 800x480, Linux
 *              vc4-kms-dsi-7inch overlay): ATTINY power controller
 *              (rpi-panel-attiny-regulator) + TC358762 DSI->DPI
 *              bridge initialized over DSI LP generic writes.
 *              1 lane, LPM, continuous clock; vc4 drives the family
 *              with sync end events (ST_END), never the declared
 *              sync pulses.
 *
 * The Pi5 DSI path lives in the RP1 southbridge instead of the
 * BCM283x SoC:
 *
 *   bcm2712_rp1_dsi_init()   RP1 MIPI1 clocks + SNPS D-PHY/DSI host
 *                            into LP-11 (command mode)
 *   panel bring-up           family-specific rails/reset/bridge setup
 *                            (I2C, or DSI LP writes to the TC358762)
 *                            while the lanes are parked
 *   bcm2712_rp1_dsi_video_start()  ArgonDPI DMA scans the framebuffer
 *                            out of host RAM, host switches to video
 *
 * The panel control I2C follows the Pi5 board dts (i2c_csi_dsi = RP1
 * i2c4 on GPIO40/41, carried inside the display FPC); the panel's DIP
 * switch position "I2C1" moves the MCU to the 40-pin header bus (RP1
 * i2c1 on GPIO2/3). Both are probed, FPC bus first.
 *
 * PANEL_WS control, mirroring panel-waveshare-dsi:
 *   probe writes:  0xc0=1, 0xc2=1, 0xac=1
 *   enable:        0xad=1
 *   backlight:     0xab = 0xff - brightness, then 0xaa = 1
 * Mode from the kernel's ws_panel_4_0_mode: 480x800 @ 50 MHz pixel
 * clock, hfp/hsw/hbp = 150/100/150, vfp/vsw/vbp = 20/100/20, RGB888,
 * 2 lanes, non-continuous HS clock (VIDEO | VIDEO_HSE |
 * CLOCK_NON_CONTINUOUS).
 *
 * PANEL_RPI7 control, mirroring rpi-panel-attiny-regulator.c +
 * tc358762.c: ATTINY PORTA/B/C rail+reset walk and PWM backlight over
 * I2C, TC358762 bridge registers programmed with 6-byte DSI generic
 * long writes in LP mode. Mode from the firmware modeline
 * (panel-raspberrypi-touchscreen.c): 800x480 @ 25.9794 MHz, 1 lane.
 *
 * The scan-out buffer is dma_alloc()ed and handed to RP1 as an RC_BAR2
 * bus address (physical + 0x1000000000), exactly like rp1_dpi.
 */

#define WS_PANEL_I2C_ADDR      0x45U
/* FPC control bus: i2c_csi_dsi = RP1 i2c4, GPIO40/41 */
#define WS_PANEL_I2C_BUS_FPC   4
#define WS_PANEL_I2C_FPC_SDA   40
#define WS_PANEL_I2C_FPC_SCL   41
/* FPC control bus on the CAM/DISP 0 connector: i2c_csi_dsi0 = RP1
 * i2c6, GPIO38/39 (video still leaves via the MIPI1 block!) */
#define WS_PANEL_I2C_BUS_C0    6
#define WS_PANEL_I2C_C0_SDA    38
#define WS_PANEL_I2C_C0_SCL    39
/* DIP switch position "I2C1": RP1 i2c1 on the 40-pin header, GPIO2/3 */
#define WS_PANEL_I2C_BUS_DIP1  1
#define WS_PANEL_I2C1_SDA      2
#define WS_PANEL_I2C1_SCL      3

/*
 * On-panel MCU power controller registers and state bits (same map the
 * raspix port uses on this exact hardware): reg 0x94 holds the high
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
/* VSDELAY(1) | RGB888 | UNK6 | VTGEN — the constant tc358762_init()
 * builds (LCDCTRL_VSDELAY(1)|LCDCTRL_RGB888|LCDCTRL_UNK6|LCDCTRL_VTGEN).
 * VTGEN means the bridge runs the panel from its OWN timing generator
 * (loaded from the LCD_* registers below), so those values and the
 * sync polarities must describe the panel exactly.
 * NOTE: EVTMODE (bit 5) is CLEAR in it, i.e. the bridge reconstructs
 * its timing from sync PULSES (HSS..HSE pairs).  Linux pairs this
 * exact value with MIPI_DSI_MODE_VIDEO_SYNC_PULSE on the Pi5 host.
 * If the host runs sync EVENTS instead, EVTMODE must be set or the
 * bridge waits for sync-end packets that never come, loses vertical
 * lock and re-scans the panel several times per incoming frame
 * (screen tiled with N stacked copies of the frame top). */
#define TC_LCDCTRL_MAGIC       0x00100150U
#define TC_LCDCTRL_EVTMODE     (1u << 5)
/* the panel mode carries DRM_MODE_FLAG_NHSYNC|NVSYNC, which
 * tc358762_init() turns into these two polarity bits — without them
 * VTGEN drives the glass with inverted syncs */
#define TC_LCDCTRL_HSPOL       (1u << 17)
#define TC_LCDCTRL_VSPOL       (1u << 19)
#define TC_SYSCTRL_MAGIC       0x040fU
#define TC_LPX_PERIOD          3U

/*
 * Built-in defaults: the kernel's ws_panel_4_0_mode. Any field can be
 * overridden from the display config when it carries "output":"dsi"
 * (same pattern as the raspix port), so other panels of the Waveshare
 * DSI family (same MCU, different timings) work without a rebuild.
 */
static bcm2712_dsi_mode_t _panel_mode = {
	.width = 480,
	.height = 800,
	.hfp = 150, .hsw = 100, .hbp = 150,
	.vfp = 20,  .vsw = 100, .vbp = 20,
	.pixel_clock_hz = 50000000U,
	.lanes = 2,
	.continuous_clock = 0,
	.sync_pulse = 0,
};

/*
 * PANEL_RPI7 mode: raspberrypi_7inch_mode from panel-simple.c, the
 * mode Linux actually feeds the TC358762 bridge path (compatible
 * "raspberrypi,7inch-dsi", used by the vc4-kms-dsi-7inch overlay):
 * 30MHz, hfp/hsw/hbp = 131/2/45 (htotal 978), vfp/vsw/vbp = 7/2/22
 * (vtotal 511) => 60.03Hz, negative H and V sync.
 *
 * The old firmware modeline (25979400Hz, hfp=1, htotal=849) belongs to
 * panel-raspberrypi-touchscreen.c, which drives the glass through its
 * own DSI host, not through this bridge: at htotal=849 the DSI link
 * budget is exactly 3 bytes per pixel (byteclk/pclk = 3.000), leaving
 * no room for packet headers/checksums/EoTp, and the bridge's VTGEN
 * gets a line far shorter than the glass needs.
 */
static const bcm2712_dsi_mode_t _rpi7_mode = {
	.width = 800,
	.height = 480,
	.hfp = 131, .hsw = 2, .hbp = 45,
	.vfp = 7,   .vsw = 2, .vbp = 22,
	.pixel_clock_hz = 30000000U,
	.lanes = 1,
	.continuous_clock = 1,
	.sync_pulse = 1,
};

#define PANEL_WS    0
#define PANEL_RPI7  1
/*
 * ClockworkPi uConsole/DevTerm panels (raspix dsi_fbdisplayd family):
 * raw MIPI DSI glass driven by a vendor DCS init table over the RP1
 * host, OCP8178 1-wire backlight on its own GPIO and AXP223 PMIC rails
 * on I2C — no control MCU at 0x45 to probe, so the conf must name them.
 *
 * cwu50 comes in two mutually exclusive hardware batches that need
 * different DDIC init tables but identical timings, hence the separate
 * PANEL_CWU50OLD kind.
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
 * Software contrast, the `ct` dev.cmd knob.  A plain centre-preserving
 * LUT applied on the flush path, so it stays inert at the default 100%
 * and never affects the ws/rpi7 families, which do not register
 * uc_dev_cmd at all.
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
static volatile int _dsi_ok = 0;
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
	 * whose DDIC was never initialised for those timings.  The blanking
	 * / pixel-clock / lane keys stay honoured so a new panel of the same
	 * family can be tried without a rebuild.
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
	_panel_mode.sync_pulse = (uint32_t)json_get_int_def(conf_var, "sync_pulse",
			(int)_panel_mode.sync_pulse);
	_panel_mode.lp_hblank = (uint32_t)json_get_int_def(conf_var, "lp_hblank",
			(int)_panel_mode.lp_hblank);
	json_var_unref(conf_var);
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
	 * LUT is a 16bpp-only feature.
	 */
	if ((uintptr_t)phy->pointer == (uintptr_t)g->buffer)
		return (uint32_t)g->w * (uint32_t)g->h * 4U;
	return blt32_pitch(phy, g);
}

static disp_info_t* get_info(void) {
	return &_fb_info;
}

/*
 * Fatal bring-up failure: dump all three RP1 MIPI1 register banks so
 * the wiring harness can be diagnosed stage by stage.
 */
static int32_t fail_stage(int stage) {
	slog("dsi_fbdisplayd: bring-up failed at stage %d\n", stage);
	bcm2712_rp1_dsi_dump();
	/*
	 * A handheld has no console to read, so also flag the stage on the
	 * backlight.  Finite: this returns and lets the daemon exit normally
	 * so init can carry on.
	 */
	if (PANEL_IS_UC(_panel_kind))
		uc_backlight_blink((uint32_t)stage);
	return -1;
}

/* ─── panel control I2C (Waveshare MCU at 0x45) ─── */

/*
 * The panel's DIP switch selects which bus the control MCU answers on:
 * FPC position = RP1 i2c4 (GPIO40/41, the Pi5 i2c_csi_dsi alias — the
 * shipping wiring), "I2C1" position = RP1 i2c1 on the 40-pin header
 * (GPIO2/3). Probe the FPC bus first and fall back to the header bus;
 * once the MCU ACKs the bus is latched, so re-inits skip the probe.
 */
static int _ws_i2c_bus = -1;

static int32_t ws_panel_i2c_probe(int bus, uint32_t sda, uint32_t scl) {
	if (bcm2712_i2c_init_pins(bus, sda, scl) != 0) {
		slog("dsi_fbdisplayd: i2c%d (GPIO%u/%u) controller init failed\n",
				bus, sda, scl);
		return -1;
	}
	/* any answered register access proves the controller is on this
	 * bus: the ATTINY answers REG_ID (0x80), the WS MCU its state
	 * pair (0x94) */
	if (bcm2712_i2c_getb(bus, WS_PANEL_I2C_ADDR, RPI7_REG_ID) >= 0 ||
			bcm2712_i2c_getb(bus, WS_PANEL_I2C_ADDR, WS_MCU_REG_TP) >= 0)
		return 0;
	slog("dsi_fbdisplayd: no MCU 0x%02x ack on i2c%d (GPIO%u/%u)\n",
			WS_PANEL_I2C_ADDR, bus, sda, scl);
	return -1;
}

static int32_t ws_panel_i2c_init(void) {
	if (_ws_i2c_bus == WS_PANEL_I2C_BUS_FPC) {
		if (ws_panel_i2c_probe(WS_PANEL_I2C_BUS_FPC,
				WS_PANEL_I2C_FPC_SDA, WS_PANEL_I2C_FPC_SCL) == 0)
			return 0;
	} else if (_ws_i2c_bus == WS_PANEL_I2C_BUS_C0) {
		if (ws_panel_i2c_probe(WS_PANEL_I2C_BUS_C0,
				WS_PANEL_I2C_C0_SDA, WS_PANEL_I2C_C0_SCL) == 0)
			return 0;
	} else if (_ws_i2c_bus == WS_PANEL_I2C_BUS_DIP1) {
		if (ws_panel_i2c_probe(WS_PANEL_I2C_BUS_DIP1,
				WS_PANEL_I2C1_SDA, WS_PANEL_I2C1_SCL) == 0)
			return 0;
	}

	if (ws_panel_i2c_probe(WS_PANEL_I2C_BUS_FPC,
			WS_PANEL_I2C_FPC_SDA, WS_PANEL_I2C_FPC_SCL) == 0) {
		_ws_i2c_bus = WS_PANEL_I2C_BUS_FPC;
		slog("dsi_fbdisplayd: panel controller on i2c%d (GPIO%d/%d)\n",
				WS_PANEL_I2C_BUS_FPC, WS_PANEL_I2C_FPC_SDA,
				WS_PANEL_I2C_FPC_SCL);
		return 0;
	}
	if (ws_panel_i2c_probe(WS_PANEL_I2C_BUS_C0,
			WS_PANEL_I2C_C0_SDA, WS_PANEL_I2C_C0_SCL) == 0) {
		_ws_i2c_bus = WS_PANEL_I2C_BUS_C0;
		slog("dsi_fbdisplayd: panel controller on i2c%d (GPIO%d/%d) - "
				"CAM/DISP 0 connector, but video goes out MIPI1!\n",
				WS_PANEL_I2C_BUS_C0, WS_PANEL_I2C_C0_SDA,
				WS_PANEL_I2C_C0_SCL);
		return 0;
	}
	if (ws_panel_i2c_probe(WS_PANEL_I2C_BUS_DIP1,
			WS_PANEL_I2C1_SDA, WS_PANEL_I2C1_SCL) == 0) {
		_ws_i2c_bus = WS_PANEL_I2C_BUS_DIP1;
		slog("dsi_fbdisplayd: panel controller on i2c%d (GPIO%d/%d)\n",
				WS_PANEL_I2C_BUS_DIP1, WS_PANEL_I2C1_SDA,
				WS_PANEL_I2C1_SCL);
		return 0;
	}

	_ws_i2c_bus = -1;
	slog("dsi_fbdisplayd: panel MCU 0x%02x on neither i2c%d (GPIO%d/%d) nor i2c%d (GPIO%d/%d) nor i2c%d (GPIO%d/%d)\n",
			WS_PANEL_I2C_ADDR,
			WS_PANEL_I2C_BUS_FPC, WS_PANEL_I2C_FPC_SDA, WS_PANEL_I2C_FPC_SCL,
			WS_PANEL_I2C_BUS_C0, WS_PANEL_I2C_C0_SDA, WS_PANEL_I2C_C0_SCL,
			WS_PANEL_I2C_BUS_DIP1, WS_PANEL_I2C1_SDA, WS_PANEL_I2C1_SCL);
	return -1;
}

/*
 * Waveshare panel control register write (smbus byte-data = the two
 * raw bytes {reg, val}).
 */
static int32_t ws_panel_i2c_write(uint8_t reg, uint8_t val) {
	uint8_t buf[2] = { reg, val };

	if (_ws_i2c_bus < 0 ||
			bcm2712_i2c_write(_ws_i2c_bus, WS_PANEL_I2C_ADDR, buf, 2) != 0) {
		slog("dsi_fbdisplayd: panel I2C write 0x%02x=0x%02x failed\n",
				reg, val);
		return -1;
	}
	return 0;
}

static int32_t ws_panel_i2c_read(uint8_t reg, uint8_t* val) {
	if (_ws_i2c_bus < 0 ||
			bcm2712_i2c_write_read(_ws_i2c_bus, WS_PANEL_I2C_ADDR,
				&reg, 1, val, 1) != 0)
		return -1;
	return 0;
}

static void ws_panel_mdelay(uint32_t ms) {
	usleep(ms * 1000U);
}

static int32_t ws_mcu_set_power(uint16_t state, uint32_t settle_ms) {
	if (ws_panel_i2c_write((uint8_t)WS_MCU_REG_TP, (uint8_t)(state >> 8)) != 0)
		return -1;
	if (ws_panel_i2c_write((uint8_t)WS_MCU_REG_LCD, (uint8_t)(state & 0xff)) != 0)
		return -1;
	ws_panel_mdelay(settle_ms);
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
 * Panel rails up BEFORE the DSI PHY goes active: driving LP-11 into an
 * unpowered DDIC parasitically powers it through the ESD diodes, so the
 * real rail arrival never triggers a clean POR and the panel latches
 * deaf to reset. The rail walk mirrors the raspix port (which mirrors
 * dsi_touchd, provably working on this hardware): VCC+ENABLE, then
 * AVDD+IOVCC. Failure is logged, not fatal: the MCU may already hold
 * the rails on from its own defaults.
 */
static void ws_panel_power(void) {
	uint16_t state = 0;

	if (ws_panel_i2c_init() != 0)
		return;
	if (ws_mcu_get_power(&state) != 0) {
		slog("dsi_fbdisplayd: mcu power sync failed\n");
		return;
	}
	state |= WS_PWR_VCC | WS_PWR_ENABLE;
	if (ws_mcu_set_power(state, 20) != 0) {
		slog("dsi_fbdisplayd: mcu vcc/enable write failed\n");
		return;
	}
	state |= WS_PWR_AVDD | WS_PWR_IOVCC;
	if (ws_mcu_set_power(state, 60) != 0) {
		slog("dsi_fbdisplayd: mcu avdd/iovcc write failed\n");
		return;
	}
}

/*
 * Reset pulse AFTER the DSI host parks the lanes in LP-11 (the raspix
 * port's pulse shape, assert 20ms / release 180ms): the DDIC comes out
 * of reset seeing LP-11 on the lanes.
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

/*
 * Panel bring-up over I2C, mirroring panel-waveshare-dsi but split the
 * way DRM splits bridge pre_enable/enable:
 *   pre_enable: probe writes (0xc0/0xc2/0xac) while the host still sits
 *               at LP-11;
 *   enable:     display-on (0xad=1) + backlight (0xab/0xaa) ONLY once
 *               the DSI video stream is live. Writing 0xad before a
 *               valid stream lets the panel latch a no-signal state
 *               (black) that only clears on its slow retry.
 */
static int32_t ws_panel_pre_enable(void) {
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

/* ---- PANEL_RPI7: ATTINY power controller + TC358762 bridge ---- */

static int32_t rpi7_bridge_write(uint16_t reg, uint32_t val) {
	uint8_t buf[6] = {
		(uint8_t)(reg & 0xffU), (uint8_t)(reg >> 8),
		(uint8_t)(val & 0xffU), (uint8_t)((val >> 8) & 0xffU),
		(uint8_t)((val >> 16) & 0xffU), (uint8_t)((val >> 24) & 0xffU),
	};
	int try, r = -1;

	/* 0x29 = generic long write, in LP mode (MIPI_DSI_MODE_LPM).
	 * r==1: the packet went out but the bridge's ACK carried an error
	 * report (e.g. false control error) — the write may have been
	 * dropped on the wire, so send it again until the ACK is clean. */
	for (try = 0; try < 3; ++try) {
		r = bcm2712_rp1_dsi_cmd_write(0x29, buf, sizeof(buf), 1);
		if (r == 0)
			return 0;
		if (r < 0)
			break;
	}
	slog("dsi_fbdisplayd: TC358762 write 0x%04x=0x%08x %s\n", reg, val,
			r < 0 ? "failed" : "still reporting errors");
	return r < 0 ? -1 : 0;
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
	ws_panel_mdelay(30);
	ws_panel_i2c_write(RPI7_REG_PWM, 0x00);
	ws_panel_mdelay(10);

	ws_panel_i2c_write(RPI7_REG_PORTC, 0x00);
	ws_panel_mdelay(10);
	ws_panel_i2c_write(RPI7_REG_PORTA, RPI7_PA_LCD_LR);
	ws_panel_mdelay(10);
	ws_panel_i2c_write(RPI7_REG_PORTB, RPI7_PB_LCD_MAIN);
	ws_panel_mdelay(10);
	ws_panel_i2c_write(RPI7_REG_PORTC, RPI7_PC_LED_EN);
	ws_panel_mdelay(80);
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
	uint32_t lcdctrl = TC_LCDCTRL_MAGIC |
			TC_LCDCTRL_HSPOL | TC_LCDCTRL_VSPOL;

	if (ws_panel_i2c_write(RPI7_REG_PORTC,
			RPI7_PC_LED_EN | RPI7_PC_RST_TP_N |
			RPI7_PC_RST_LCD_N | RPI7_PC_RST_BRIDGE_N) != 0)
		return -1;
	ws_panel_mdelay(8);
	ws_panel_i2c_write(RPI7_REG_ADDR_H, 0x04);
	ws_panel_mdelay(8);
	ws_panel_i2c_write(RPI7_REG_ADDR_L, 0x7c);
	ws_panel_mdelay(8);
	ws_panel_i2c_write(RPI7_REG_WRITE_H, 0x00);
	ws_panel_mdelay(8);
	ws_panel_i2c_write(RPI7_REG_WRITE_L, 0x00);
	ws_panel_mdelay(100);

	/* absorb the error flags the bridge accumulated while the host
	 * PHY was coming up (LP glitches during lane bring-up latch a
	 * false-control error): SMRPS is a pure protocol command with no
	 * register side effect, its ACK carries the stale report away so
	 * every following write's ACK speaks only for itself */
	bcm2712_rp1_dsi_cmd_short(0x37, 4, 0, 1);

	rc |= rpi7_bridge_write(TC_DSI_LANEENABLE,
			TC_LANEENABLE_CLEN | TC_LANEENABLE_L0EN);
	rc |= rpi7_bridge_write(TC_PPI_D0S_CLRSIPOCOUNT, 5);
	rc |= rpi7_bridge_write(TC_PPI_D1S_CLRSIPOCOUNT, 5);
	rc |= rpi7_bridge_write(TC_PPI_D0S_ATMR, 0);
	rc |= rpi7_bridge_write(TC_PPI_D1S_ATMR, 0);
	rc |= rpi7_bridge_write(TC_PPI_LPTXTIMECNT, TC_LPX_PERIOD);
	rc |= rpi7_bridge_write(TC_SPICMR, 0x00);
	/* the bridge's sync flavour must match the host's
	 * VID_MODE_CFG: pulse mode (EVTMODE clear) for sync_pulse=1,
	 * event mode (EVTMODE set) for sync events — a mismatch
	 * desyncs VTGEN into repeated frame bands */
	if (!_panel_mode.sync_pulse)
		lcdctrl |= TC_LCDCTRL_EVTMODE;
	rc |= rpi7_bridge_write(TC_LCDCTRL, lcdctrl);
	rc |= rpi7_bridge_write(TC_SYSCTRL, TC_SYSCTRL_MAGIC);
	rc |= rpi7_bridge_write(TC_LCD_HS_HBP,
			_panel_mode.hsw | (_panel_mode.hbp << 16));
	rc |= rpi7_bridge_write(TC_LCD_HDISP_HFP,
			_panel_mode.width | (_panel_mode.hfp << 16));
	rc |= rpi7_bridge_write(TC_LCD_VS_VBP,
			_panel_mode.vsw | (_panel_mode.vbp << 16));
	rc |= rpi7_bridge_write(TC_LCD_VDISP_VFP,
			_panel_mode.height | (_panel_mode.vfp << 16));
	ws_panel_mdelay(100);

	rc |= rpi7_bridge_write(TC_PPI_STARTPPI, 1);
	rc |= rpi7_bridge_write(TC_DSI_STARTDSI, 1);
	ws_panel_mdelay(100);

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
 * timing.  Needs the I2C mmio up, so this runs inside init().
 */
static void panel_setup(void) {
	uint8_t id = 0xff;

	if (_panel_kind_conf >= 0) {
		_panel_kind = _panel_kind_conf;
	} else {
		_panel_kind = PANEL_WS;
		if (ws_panel_i2c_init() != 0) {
			slog("dsi_fbdisplayd: no controller on either i2c bus\n");
		} else if (ws_panel_i2c_read(RPI7_REG_ID, &id) != 0) {
			slog("dsi_fbdisplayd: controller answers but REG_ID read failed\n");
		} else if (id == RPI7_ID_V1 || id == RPI7_ID_V2) {
			_panel_kind = PANEL_RPI7;
			if (!_conf_has_mode)
				_panel_mode = _rpi7_mode;
		} else {
			slog("dsi_fbdisplayd: controller id 0x%02x (not attiny), assuming ws\n",
					id);
		}
	}
	if (_ws_i2c_bus >= 0)
		slog("dsi_fbdisplayd: panel %s %ux%u (i2c bus %d)\n",
				_panel_kind_name(_panel_kind),
				_panel_mode.width, _panel_mode.height, _ws_i2c_bus);
	else
		slog("dsi_fbdisplayd: panel %s %ux%u (i2c bus probing)\n",
				_panel_kind_name(_panel_kind),
				_panel_mode.width, _panel_mode.height);
}

/* ─── scan-out buffer ─── */

/*
 * Allocate the framebuffer through the DMA heap and hand RP1 its RC_BAR2
 * bus address (physical + 0x1000000000), exactly like rp1_dpi: RP1
 * reads host RAM through the PCIe inbound window.
 */
static int32_t map_scanout_buffer(uint32_t w, uint32_t h, uint32_t dep) {
	sys_info_t sysinfo;
	uint32_t bpp = dep / 8U;
	uint32_t pitch = w * bpp;
	uint32_t size = pitch * h;
	uint32_t page_size, alloc_size;
	ewokos_addr_t fb_vaddr, fb_phy;

	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
	page_size = sysinfo.page_size == 0 ? 4096U : sysinfo.page_size;
	alloc_size = (size + page_size - 1U) & ~(page_size - 1U);

	fb_vaddr = dma_alloc(0, alloc_size);
	if (fb_vaddr == 0) {
		slog("dsi_fbdisplayd: dma alloc failed size=%u\n", alloc_size);
		return -1;
	}
	fb_phy = dma_phy_addr(0, fb_vaddr);
	if (fb_phy == 0) {
		slog("dsi_fbdisplayd: bad dma phy\n");
		dma_free(0, fb_vaddr);
		return -1;
	}

	memset(&_fb_info, 0, sizeof(_fb_info));
	_fb_info.width = w;
	_fb_info.height = h;
	_fb_info.vwidth = w;
	_fb_info.vheight = h;
	_fb_info.depth = dep;
	_fb_info.pitch = pitch;
	_fb_info.pointer = fb_vaddr;
	_fb_info.phy_base = fb_phy;
	_fb_info.bus_base = fb_phy + 0x1000000000ULL;
	_fb_info.size = size;
	_fb_info.size_max = alloc_size;
	_fb_info.xoffset = 0;
	_fb_info.yoffset = 0;
	_fb_info.dma_id = 0;
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

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
	int st;

	if (dep != 16 && dep != 32)
		dep = 32;

	/*
	 * Resolve the panel family (the I2C probe maps the RP1 window),
	 * then force the scan-out geometry to the physical panel
	 * regardless of what the config said.
	 */
	panel_setup();
	w = _panel_mode.width;
	h = _panel_mode.height;

	/*
	 * Panel rails FIRST, before any DSI PHY activity (see
	 * ws_panel_power): a cold panel must see its rails up before
	 * the lanes go LP-11.  The ClockworkPi families bring up their
	 * AXP223 rails, OCP8178 backlight and reset GPIO here instead.
	 */
	if (PANEL_IS_UC(_panel_kind))
		uc_panel_prepare();
	else if (_panel_kind == PANEL_RPI7)
		rpi7_panel_power();
	else
		ws_panel_power();

	/* clocks, D-PHY and host; lanes end at LP-11, host in command mode */
	if (bcm2712_rp1_dsi_init(&_panel_mode) != 0)
		return fail_stage(2);

	/*
	 * Lanes are at LP-11.
	 *   UC:    map the scan-out window first (keep the dma_alloc
	 *          syscall out of the DCS sequence), then the reset pulse
	 *          and the vendor DCS init table — both at LP-11, both
	 *          before the pipeline starts pushing pixels.
	 *   RPI7:  bridge out of reset plus the full TC358762 register
	 *          init over DSI LP writes (backlight still waits for the
	 *          live stream).
	 *   WS:    DDIC reset pulse so it comes out of reset into a live
	 *          bus.
	 */
	if (PANEL_IS_UC(_panel_kind) && map_scanout_buffer(w, h, dep) != 0)
		return fail_stage(4);

	if (PANEL_IS_UC(_panel_kind)) {
		uc_panel_reset_pulse();
		if (uc_panel_init_table(_uc_family(_panel_kind)) != 0)
			return fail_stage(6);
	} else if (_panel_kind == PANEL_RPI7) {
		if (rpi7_bridge_enable() != 0)
			return fail_stage(3);
	} else if (ws_panel_reset_pulse() != 0)
		slog("dsi_fbdisplayd: WARN panel reset pulse failed\n");

	if (!PANEL_IS_UC(_panel_kind) && map_scanout_buffer(w, h, dep) != 0)
		return fail_stage(4);

	/* WS bridge pre_enable while the host sits at LP-11; the rpi7
	 * bridge init already ran above */
	if (_panel_kind == PANEL_WS && ws_panel_pre_enable() != 0)
		return fail_stage(5);

	/* prime the scan-out buffer BEFORE the pipeline starts fetching */
	fill_black(&_fb_info);

	/* arm the DMA engine and switch the host to video mode */
	if (bcm2712_rp1_dsi_video_start(_fb_info.bus_base, _fb_info.pitch,
			w, h, dep) != 0)
		return fail_stage(6);

	/* runtime evidence: the engine must be busy scanning out */
	usleep(200000);
	st = bcm2712_rp1_dsi_check();
	if (st < 0)
		return fail_stage(7);
	slog("dsi_fbdisplayd: video stream live %ux%u depth=%u pitch=%u\n",
			_fb_info.width, _fb_info.height, _fb_info.depth,
			_fb_info.pitch);

	/*
	 * Stream is live: display-on (PANEL_WS) or backlight PWM
	 * (PANEL_RPI7) against a valid video stream, so the panel locks
	 * immediately instead of latching no-signal.  The ClockworkPi
	 * panels need nothing here — SLPOUT/DSPON already ran at the tail
	 * of the DCS table and the backlight was lit in uc_panel_prepare().
	 */
	if (PANEL_IS_UC(_panel_kind)) {
		/* nothing to do */
	} else if (_panel_kind == PANEL_RPI7) {
		if (rpi7_backlight() != 0)
			return fail_stage(5);
	} else if (ws_panel_enable() != 0)
		return fail_stage(5);

	_dsi_ok = 1;
	return 0;
}

/*
 * Watchdog thread: independent of any GUI redraws, polls the DSI scanout
 * engine once per second. bcm2712_rp1_dsi_check() logs a status snapshot
 * and restarts the engine if it ever stops.
 */
static void* dsi_watchdog(void* arg) {
	(void)arg;
	while (!_dsi_ok)
		usleep(100000);
	while (1) {
		sleep(1);
		bcm2712_rp1_dsi_check();
	}
	return NULL;
}

int main(int argc, char** argv) {
	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti] : "/dev/disp0";

	load_panel_conf(_conf_file);
	if (PANEL_IS_UC(_panel_kind_conf))
		fbdisplayd_set_dev_cmd(uc_dev_cmd);

	pthread_t th;
	pthread_create(&th, NULL, dsi_watchdog, NULL);

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
