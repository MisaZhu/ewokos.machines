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

/*
 * Waveshare 4inch DSI LCD (480x800) framebuffer daemon for Raspberry
 * Pi 3 (BCM2837) and Pi 4 (BCM2711).
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
 * Panel control is I2C-only (no DCS over DSI, no reset GPIO),
 * mirroring the Linux panel-waveshare-dsi driver:
 *   probe writes:  0xc0=1, 0xc2=1, 0xac=1
 *   enable:        0xad=1
 *   backlight:     0xab = 0xff - brightness, then 0xaa = 1
 *
 * Mode from the kernel's ws_panel_4_0_mode: 480x800 @ 50 MHz pixel
 * clock, hfp/hsw/hbp = 150/100/150, vfp/vsw/vbp = 20/100/20, RGB888,
 * 2 lanes, non-continuous HS clock (VIDEO | VIDEO_HSE |
 * CLOCK_NON_CONTINUOUS).
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

static const bcm283x_dsi1_mode_t _panel_mode = {
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
	_fb_info.phy_base = pick_fb_phys_base();
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
static void fill_black(const fbinfo_t* fbi) {
	if (fbi == NULL || fbi->pointer == 0)
		return;
	memset((void*)(uintptr_t)fbi->pointer, 0, fbi->size);
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
 * The panel has no reset GPIO and no DCS path; BSC0 on GPIO44/45
 * (ALTF1) is the only control channel — the same bus the kernel's
 * i2c_csi_dsi node uses.
 */
static int32_t ws_panel_pre_enable(void) {
	if (bcm283x_i2c0_init(WS_PANEL_I2C_SDA_GPIO, WS_PANEL_I2C_SCL_GPIO) != 0) {
		printf("dsi_fbdisplayd: BSC0 (GPIO%d/%d) init failed\n",
				WS_PANEL_I2C_SDA_GPIO, WS_PANEL_I2C_SCL_GPIO);
		return -1;
	}

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

	if (bcm283x_i2c0_init(WS_PANEL_I2C_SDA_GPIO, WS_PANEL_I2C_SCL_GPIO) != 0) {
		printf("dsi_fbdisplayd: BSC0 (GPIO%d/%d) init failed\n",
				WS_PANEL_I2C_SDA_GPIO, WS_PANEL_I2C_SCL_GPIO);
		return;
	}
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

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
	bcm283x_dsi1_adjusted_mode_t adj;
	int st;

	/*
	 * The scan-out geometry must match the physical panel, so force
	 * w/h to the panel mode regardless of what the config said.
	 */
	w = _panel_mode.width;
	h = _panel_mode.height;
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
	 * Panel rails FIRST, before any DSI register/PHY activity
	 * (see ws_panel_power): a cold DDIC must see its rails up
	 * before the lanes go LP-11.
	 */
	ws_panel_power();

	/*
	 * Firmware power domains: upstream Linux never power-cycles
	 * the DSI domains (no bcm27xx dtsi carries power-domains on
	 * &dsi0/&dsi1).  The only transition uconsole provably uses on
	 * real firmware is the VIDEO_SCALER (HVS) one, so enable just
	 * that when reported OFF; the port probe below is the real
	 * arbiter for everything else.
	 */
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
	{
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
	 */
	bcm283x_dsi1_firmware_handoff();

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

	/*
	 * Lanes are at LP-11: give the DDIC its reset pulse so it
	 * comes out of reset into a live bus, then the bridge
	 * pre_enable writes (display-on comes later, see below).
	 */
	if (ws_panel_reset_pulse() != 0)
		printf("dsi_fbdisplayd: WARN panel reset pulse failed\n");

	if (map_scanout_buffer(w, h, dep) != 0)
		return fail_stage(5);

	/*
	 * DRM bridge pre_enable while the host sits at LP-11: the
	 * panel is probed/configured but NOT displayed yet.  The
	 * display-on + backlight writes wait for a live video stream
	 * (ws_panel_enable() after stage 8) — enabling the bridge
	 * against a dead stream latches a no-signal black state.
	 */
	if (ws_panel_pre_enable() != 0)
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
	bcm283x_dsi1_video_mode(adj.pix_clk_divider);
	bcm283x_dsi1_pv_video_enable();

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

	/*
	 * Stream is provably live: DRM bridge enable step.  The panel
	 * now sees display-on against a valid video stream and locks
	 * immediately instead of latching no-signal.
	 */
	if (ws_panel_enable() != 0)
		return fail_stage(6);

	return 0;
}

int main(int argc, char** argv) {
	fbdisplayd_t fbdisplayd;
	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti] : "/dev/disp0";

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
