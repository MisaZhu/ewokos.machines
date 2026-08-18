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

/*
 * Waveshare DSI LCD framebuffer daemon for Raspberry Pi 5 (BCM2712).
 *
 * Port of the raspix dsi_fbdisplayd: same Waveshare MCU panel family
 * (Linux panel-waveshare-dsi, I2C-only control at 0x45, no DCS over
 * DSI, no reset GPIO), but the Pi5 DSI path lives in the RP1
 * southbridge instead of the BCM283x SoC:
 *
 *   bcm2712_rp1_dsi_init()   RP1 MIPI1 clocks + SNPS D-PHY/DSI host
 *                            into LP-11 (command mode)
 *   ws_panel_power/reset     panel rails + DDIC reset over the panel
 *                            control I2C while the lanes are parked
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
 * The scan-out buffer is dma_alloc()ed and handed to RP1 as an RC_BAR2
 * bus address (physical + 0x1000000000), exactly like rp1_dpi.
 */

#define WS_PANEL_I2C_ADDR      0x45U
/* FPC control bus: i2c_csi_dsi = RP1 i2c4, GPIO40/41 */
#define WS_PANEL_I2C_BUS_FPC   4
#define WS_PANEL_I2C_FPC_SDA   40
#define WS_PANEL_I2C_FPC_SCL   41
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
};

static const char* _conf_file = "";
static int _display_index = 0;
static fbinfo_t _fb_info;
static volatile int _dsi_ok = 0;

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

	_panel_mode.width  = (uint32_t)json_get_int_def(conf_var, "width",  (int)_panel_mode.width);
	_panel_mode.height = (uint32_t)json_get_int_def(conf_var, "height", (int)_panel_mode.height);
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

	if (phy == NULL || phy->pointer == 0)
		return 0;
	if (phy->depth != 32 && phy->depth != 16)
		return 0;

	if (phy->depth == 16)
		return blt16_pitch(phy, g);

	/* Zero-copy fast path: rendering already targets the framebuffer. */
	if ((uintptr_t)phy->pointer == (uintptr_t)g->buffer)
		return (uint32_t)g->w * (uint32_t)g->h * 4U;
	return blt32_pitch(phy, g);
}

static fbinfo_t* get_info(void) {
	return &_fb_info;
}

/*
 * Fatal bring-up failure: dump all three RP1 MIPI1 register banks so
 * the wiring harness can be diagnosed stage by stage.
 */
static int32_t fail_stage(int stage) {
	printf("dsi_fbdisplayd: bring-up failed at stage %d\n", stage);
	bcm2712_rp1_dsi_dump();
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
	if (bcm2712_i2c_init_pins(bus, sda, scl) != 0)
		return -1;
	/* any answered register access proves the MCU is on this bus */
	if (bcm2712_i2c_getb(bus, WS_PANEL_I2C_ADDR, WS_MCU_REG_LCD) < 0)
		return -1;
	return 0;
}

static int32_t ws_panel_i2c_init(void) {
	if (_ws_i2c_bus == WS_PANEL_I2C_BUS_FPC) {
		if (ws_panel_i2c_probe(WS_PANEL_I2C_BUS_FPC,
				WS_PANEL_I2C_FPC_SDA, WS_PANEL_I2C_FPC_SCL) == 0)
			return 0;
	} else if (_ws_i2c_bus == WS_PANEL_I2C_BUS_DIP1) {
		if (ws_panel_i2c_probe(WS_PANEL_I2C_BUS_DIP1,
				WS_PANEL_I2C1_SDA, WS_PANEL_I2C1_SCL) == 0)
			return 0;
	}

	if (ws_panel_i2c_probe(WS_PANEL_I2C_BUS_FPC,
			WS_PANEL_I2C_FPC_SDA, WS_PANEL_I2C_FPC_SCL) == 0) {
		_ws_i2c_bus = WS_PANEL_I2C_BUS_FPC;
		return 0;
	}
	if (ws_panel_i2c_probe(WS_PANEL_I2C_BUS_DIP1,
			WS_PANEL_I2C1_SDA, WS_PANEL_I2C1_SCL) == 0) {
		_ws_i2c_bus = WS_PANEL_I2C_BUS_DIP1;
		return 0;
	}

	_ws_i2c_bus = -1;
	printf("dsi_fbdisplayd: panel MCU 0x%02x on neither i2c%d (GPIO%d/%d) nor i2c%d (GPIO%d/%d)\n",
			WS_PANEL_I2C_ADDR,
			WS_PANEL_I2C_BUS_FPC, WS_PANEL_I2C_FPC_SDA, WS_PANEL_I2C_FPC_SCL,
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
		printf("dsi_fbdisplayd: panel I2C write 0x%02x=0x%02x failed\n",
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
		printf("dsi_fbdisplayd: dma alloc failed size=%u\n", alloc_size);
		return -1;
	}
	fb_phy = dma_phy_addr(0, fb_vaddr);
	if (fb_phy == 0) {
		printf("dsi_fbdisplayd: bad dma phy\n");
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
static void fill_black(const fbinfo_t* fbi) {
	if (fbi == NULL || fbi->pointer == 0)
		return;
	memset((void*)(uintptr_t)fbi->pointer, 0, fbi->size);
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
	int st;

	if (dep != 16 && dep != 32)
		dep = 32;

	/* the scan-out geometry follows the physical panel */
	w = _panel_mode.width;
	h = _panel_mode.height;

	/*
	 * Panel rails FIRST, before any DSI PHY activity (see
	 * ws_panel_power). The MCU's I2C init also maps the RP1 window,
	 * so this doubles as the liveness probe for the panel bus.
	 */
	ws_panel_power();

	/* clocks, D-PHY and host; lanes end at LP-11, host in command mode */
	if (bcm2712_rp1_dsi_init(&_panel_mode) != 0)
		return fail_stage(2);

	/* DDIC reset pulse so it comes out of reset into a live bus */
	if (ws_panel_reset_pulse() != 0)
		printf("dsi_fbdisplayd: WARN panel reset pulse failed\n");

	if (map_scanout_buffer(w, h, dep) != 0)
		return fail_stage(4);

	/* bridge pre_enable while the host sits at LP-11 */
	if (ws_panel_pre_enable() != 0)
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

	/* stream is live: display-on + backlight against valid video */
	if (ws_panel_enable() != 0)
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
	fbdisplayd_t fbdisplayd;
	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti] : "/dev/disp0";

	load_panel_conf(_conf_file);

	pthread_t th;
	pthread_create(&th, NULL, dsi_watchdog, NULL);

	memset(&fbdisplayd, 0, sizeof(fbdisplayd));
	fbdisplayd.splash = NULL;   /* default logo splash from libdisplayd */
	fbdisplayd.flush = flush;
	/* flush is a plain blit into the scan-out buffer, so the
	 * libdisplayd generic direct-to-fb rotation applies. */
	fbdisplayd.flush_rotate = fbdisplayd_rotate_to;
	/* Non-rotated scan-out is also a plain memory blit, so
	 * libdisplayd can push just the dirty rects. */
	fbdisplayd_set_flush_rect(fbdisplayd_flush_rect_to);
	fbdisplayd.init = init;
	fbdisplayd.get_info = get_info;

	return fbdisplayd_run(&fbdisplayd, mnt_point,
			_panel_mode.width, _panel_mode.height,
			_conf_file, _display_index);
}
