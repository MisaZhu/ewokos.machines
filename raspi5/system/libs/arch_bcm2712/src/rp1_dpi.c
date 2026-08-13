/*
 * RP1 DPI-DMA display output for BCM2712 (Raspberry Pi 5).
 *
 * The parallel display interface of the Pi5 lives inside the RP1
 * southbridge, not in the BCM2712: the DPI-DMA block (0x148000 in the RP1
 * register file) generates the pixel timing and DMAs the framebuffer
 * straight from host RAM, VIDEO_OUT_CFG (0x140000) routes the signals to
 * the GPIO pads, and the pixel clock comes from RP1's video PLL tree
 * (clk-rp1.c register layout). Register programming follows
 * raspberrypi/linux drivers/gpu/drm/rp1/rp1-dpi/.
 *
 * Output wiring is the 24-bit "mode 7" variant from rp1.dtsi: GPIO0..3
 * carry PCLK, HSYNC, VSYNC and DE, GPIO4..27 carry D0..D23, all on RP1
 * funcsel 1. The framebuffer is read by RP1 over PCIe, so its DMA address
 * is the host physical address plus the RC_BAR2 inbound window offset.
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <ewoksys/syscall.h>
#include <arch/bcm2712/mmio.h>
#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/rp1.h>
#include <arch/bcm2712/native_hdmi.h>
#include <arch/bcm2712/rp1_dpi.h>

#define RP1_XOSC_HZ		50000000U

/* RP1 blocks used by DPI, offsets inside the RP1 window */
#define RP1_CLOCKS_OFF		(PI5_RP1_WIN_OFF + 0x00018000)
#define RP1_VIDOUT_CFG_OFF	(PI5_RP1_WIN_OFF + 0x00140000)
#define RP1_DPI_DMA_OFF		(PI5_RP1_WIN_OFF + 0x00148000)

/* ─── video PLL (PLL_VIDEO core), relative to RP1_CLOCKS_OFF ─── */

#define PLL_VIDEO_CS		0x10000
#define PLL_VIDEO_PWR		0x10004
#define PLL_VIDEO_FBDIV_INT	0x10008
#define PLL_VIDEO_FBDIV_FRAC	0x1000c
#define PLL_VIDEO_PRIM		0x10010

#define PLL_CS_LOCK		(1U << 31)
#define PLL_CS_REFDIV_LSB	0
#define PLL_PWR_DSMPD		(1U << 2)
#define PLL_PWR_MASK		0x0000003fU
#define PLL_PRIM_DIV1_LSB	16
#define PLL_PRIM_DIV1_MASK	0x00070000U
#define PLL_PRIM_DIV2_LSB	12
#define PLL_PRIM_DIV2_MASK	0x00007000U

/* ─── clk_dpi gate/divider (VIDEO_CLOCKS_OFFSET), relative to clocks ─── */

#define CLK_DPI_CTRL		(0x4000 + 0x0010)
#define CLK_DPI_DIV_INT		(0x4000 + 0x0014)

#define CLK_CTRL_ENABLE		(1U << 11)
#define CLK_CTRL_AUXSRC_MASK	0x000003e0U
#define CLK_CTRL_AUXSRC_LSB	5
#define CLK_CTRL_SRC_LSB	0
#define CLK_CTRL_SRC_AUX	1U
#define CLK_DPI_AUXSRC_PLL_VIDEO 2U  /* aux parent #2 of clk_dpi: pll_video */

#define PLL_LOCK_POLL_US	10U
#define PLL_LOCK_TIMEOUT_US	100000U

/* ─── VIDEO_OUT_CFG block ─── */

#define VIDOUT_CFG_SEL		0x0000
#define VIDOUT_CFG_SEL_PCLK_INV	(1U << 4)
#define VIDOUT_CFG_VDAC_CFG	0x0004
#define VIDOUT_CFG_MEM_PD	0x000c
#define VIDOUT_CFG_MEM_PD_VEC	(1U << 1)
#define VIDOUT_CFG_TEST_OVERRIDE 0x0010
#define VIDOUT_TEST_OVR_VDAC	(1U << 30)
#define VIDOUT_CFG_INTE		0x0018

/* ─── DPI-DMA block (rp1_dpi_hw.c register map) ─── */

#define DPI_DMA_CONTROL		0x00
#define DPI_DMA_CTRL_ARM		(1U << 0)
#define DPI_DMA_CTRL_AUTO_REPEAT	(1U << 1)
#define DPI_DMA_CTRL_HIGH_WATER_LSB	3
#define DPI_DMA_CTRL_DEN_POL		(1U << 12)
#define DPI_DMA_CTRL_HSYNC_POL		(1U << 13)
#define DPI_DMA_CTRL_VSYNC_POL		(1U << 14)
#define DPI_DMA_CTRL_HBP_EN		(1U << 17)
#define DPI_DMA_CTRL_HFP_EN		(1U << 18)
#define DPI_DMA_CTRL_VBP_EN		(1U << 19)
#define DPI_DMA_CTRL_VFP_EN		(1U << 20)
#define DPI_DMA_CTRL_HSYNC_EN		(1U << 21)
#define DPI_DMA_CTRL_VSYNC_EN		(1U << 22)

#define DPI_DMA_IRQ_EN		0x04
#define DPI_DMA_IRQ_FLAGS	0x08
#define DPI_DMA_QOS		0x0c
#define DPI_DMA_DMA_ADDR_L	0x10
#define DPI_DMA_DMA_STRIDE	0x14
#define DPI_DMA_VISIBLE_AREA	0x18
#define DPI_DMA_SYNC_WIDTH	0x1c
#define DPI_DMA_BACK_PORCH	0x20
#define DPI_DMA_FRONT_PORCH	0x24
#define DPI_DMA_SHIFT		0x28
#define DPI_DMA_IMASK		0x2c
#define DPI_DMA_OMASK		0x30
#define DPI_DMA_RGBSZ		0x34
#define DPI_DMA_PANICS		0x38
#define DPI_DMA_STATUS		0x3c
#define DPI_DMA_DMA_ADDR_H	0x40

#define DPI_DMA_STATUS_BUSY_MASK 0x00000f8fU

static ewokos_addr_t _clk_base;
static ewokos_addr_t _cfg_base;
static ewokos_addr_t _dpi_base;
static int _dpi_ready;
static uint64_t _dpi_bus_addr;
static uint32_t _dpi_stride;

static inline uint32_t dpi_min(uint32_t a, uint32_t b) {
	return a < b ? a : b;
}

static inline uint32_t abs_diff(uint32_t a, uint32_t b) {
	return a > b ? a - b : b - a;
}

/*
 * Same window setup as the other RP1 users (i2c, spi, uartd): map the main
 * MMIO window plus the RP1 register file. The RP1 window is not covered by
 * mmio_map(), touching it unmapped aborts the driver.
 */
static int rp1_dpi_map_windows(void) {
	sys_info_t sysinfo;

	if (_dpi_ready)
		return 0;

	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
	_mmio_base = sysinfo.mmio.v_base;

	if (syscall3(SYS_MEM_MAP,
			(ewokos_addr_t)sysinfo.mmio.v_base,
			(ewokos_addr_t)sysinfo.mmio.phy_base,
			(ewokos_addr_t)sysinfo.mmio.size) != sysinfo.mmio.v_base) {
		slog("rp1-dpi: main mmio map failed\n");
		return -1;
	}
	if (syscall3(SYS_MEM_MAP,
			_mmio_base + PI5_RP1_WIN_OFF,
			(ewokos_addr_t)PI5_RP1_PHY,
			(ewokos_addr_t)PI5_RP1_WIN_SIZE) != _mmio_base + PI5_RP1_WIN_OFF) {
		slog("rp1-dpi: RP1 window map failed\n");
		return -1;
	}

	_clk_base = _mmio_base + RP1_CLOCKS_OFF;
	_cfg_base = _mmio_base + RP1_VIDOUT_CFG_OFF;
	_dpi_base = _mmio_base + RP1_DPI_DMA_OFF;
	_dpi_ready = 1;
	return 0;
}

/* ─── clocks ─── */

/*
 * Choose the PLL tree for a pixel clock, mirroring rp1dpi_pipe_enable():
 *   fpix clamped to [1MHz, 200MHz]
 *   fdiv = fpix doubled into [100MHz, 200MHz]   (PLL primary output)
 *   fvco = fdiv * 2 * ceil(500M/fdiv)           (VCO in ~[1GHz, 1.33GHz])
 */
static void rp1_dpi_choose_clocks(uint32_t fpix,
		uint32_t *out_fpix, uint32_t *out_fdiv, uint32_t *out_fvco) {
	uint32_t fdiv;
	uint32_t fvco;

	if (fpix < 1000000U)
		fpix = 1000000U;
	if (fpix > 200000000U)
		fpix = 200000000U;

	fdiv = fpix;
	while (fdiv < 100000000U)
		fdiv *= 2U;
	fvco = fdiv * 2U * ((500000000U + fdiv - 1U) / fdiv);

	*out_fpix = fpix;
	*out_fdiv = fdiv;
	*out_fvco = fvco;
}

static int rp1_dpi_wait_pll_lock(void) {
	for (uint32_t waited = 0; waited < PLL_LOCK_TIMEOUT_US;
			waited += PLL_LOCK_POLL_US) {
		if (get32(_clk_base + PLL_VIDEO_CS) & PLL_CS_LOCK)
			return 0;
		usleep(PLL_LOCK_POLL_US);
	}
	slog("rp1-dpi: PLL_VIDEO failed to lock cs=%08x\n",
			get32(_clk_base + PLL_VIDEO_CS));
	return -1;
}

/*
 * Program PLL_VIDEO and clk_dpi for the given pixel clock, but leave the
 * clk_dpi gate closed; rp1_dpi_clocks_enable() opens it once the pads and
 * the VIDOUT mux are ready. Sequence follows clk-rp1.c's set_rate ops.
 */
static int rp1_dpi_clocks_setup(uint32_t fpix, uint32_t *actual_fpix) {
	uint32_t fdiv, fvco;
	uint64_t div_fp64;
	uint32_t fbdiv_int, fbdiv_frac;
	uint32_t best_div1 = 7, best_div2 = 7, best_diff = 0xffffffffU;
	uint32_t div1, div2;
	uint32_t dpi_div;
	uint64_t vco_actual, pll_out_actual;

	rp1_dpi_choose_clocks(fpix, &fpix, &fdiv, &fvco);

	/* fbdiv of the VCO against the 50MHz xosc, 24-bit fraction */
	div_fp64 = (((uint64_t)fvco << 32) + RP1_XOSC_HZ / 2) / RP1_XOSC_HZ;
	div_fp64 += 1ULL << 7;	/* round at the 24th fraction bit */
	fbdiv_int = (uint32_t)(div_fp64 >> 32);
	fbdiv_frac = (uint32_t)((div_fp64 >> 8) & 0xffffffU);

	/* primary post-divider pair (div2 <= div1 <= 7) closest to fdiv */
	for (div1 = 1; div1 <= 7; div1++) {
		for (div2 = 1; div2 <= div1; div2++) {
			uint32_t diff = abs_diff(fvco / (div1 * div2), fdiv);
			if (diff < best_diff ||
					(diff == best_diff && div1 * div2 < best_div1 * best_div2)) {
				best_diff = diff;
				best_div1 = div1;
				best_div2 = div2;
			}
		}
	}

	/* gate clk_dpi before reprogramming its divider/parent */
	put32(_clk_base + CLK_DPI_CTRL,
			get32(_clk_base + CLK_DPI_CTRL) & ~CLK_CTRL_ENABLE);

	/* reset the PLL core to a known state when it is not locked */
	if (!(get32(_clk_base + PLL_VIDEO_CS) & PLL_CS_LOCK)) {
		put32(_clk_base + PLL_VIDEO_PWR, PLL_PWR_MASK);
		put32(_clk_base + PLL_VIDEO_FBDIV_INT, 20);
		put32(_clk_base + PLL_VIDEO_FBDIV_FRAC, 0);
		put32(_clk_base + PLL_VIDEO_CS, 1U << PLL_CS_REFDIV_LSB);
	}

	/* rp1_pll_core_set_rate(): clear dividers, then set and power up */
	put32(_clk_base + PLL_VIDEO_FBDIV_INT, 0);
	put32(_clk_base + PLL_VIDEO_FBDIV_FRAC, 0);
	put32(_clk_base + PLL_VIDEO_PWR, fbdiv_frac ? 0U : PLL_PWR_DSMPD);
	put32(_clk_base + PLL_VIDEO_FBDIV_INT, fbdiv_int);
	put32(_clk_base + PLL_VIDEO_FBDIV_FRAC, fbdiv_frac);
	put32(_clk_base + PLL_VIDEO_CS,
			get32(_clk_base + PLL_VIDEO_CS) | (1U << PLL_CS_REFDIV_LSB));

	if (rp1_dpi_wait_pll_lock() != 0)
		return -1;

	/* primary divider */
	put32(_clk_base + PLL_VIDEO_PRIM,
			(get32(_clk_base + PLL_VIDEO_PRIM) &
			 ~(PLL_PRIM_DIV1_MASK | PLL_PRIM_DIV2_MASK)) |
			(best_div1 << PLL_PRIM_DIV1_LSB) |
			(best_div2 << PLL_PRIM_DIV2_LSB));

	/* clk_dpi divider and parent (pll_video via the aux mux) */
	vco_actual = ((uint64_t)RP1_XOSC_HZ *
			(((uint64_t)fbdiv_int << 24) + fbdiv_frac) + (1U << 23)) >> 24;
	pll_out_actual = vco_actual / (best_div1 * best_div2);
	dpi_div = (uint32_t)((pll_out_actual + fpix / 2) / fpix);
	if (dpi_div == 0)
		dpi_div = 1;
	if (dpi_div > 0xff)
		dpi_div = 0xff;
	put32(_clk_base + CLK_DPI_DIV_INT, dpi_div);
	put32(_clk_base + CLK_DPI_CTRL,
			(get32(_clk_base + CLK_DPI_CTRL) &
			 ~(CLK_CTRL_AUXSRC_MASK | (1U << CLK_CTRL_SRC_LSB))) |
			(CLK_DPI_AUXSRC_PLL_VIDEO << CLK_CTRL_AUXSRC_LSB) |
			(CLK_CTRL_SRC_AUX << CLK_CTRL_SRC_LSB));

	*actual_fpix = (uint32_t)(pll_out_actual / dpi_div);
	slog("rp1-dpi: pll vco=%llu fbdiv=%u.%02u prim=%ux%u dpi_div=%u\n",
			(unsigned long long)vco_actual, fbdiv_int,
			(fbdiv_frac * 100U) >> 24, best_div1, best_div2, dpi_div);
	return 0;
}

static void rp1_dpi_clocks_enable(void) {
	put32(_clk_base + CLK_DPI_CTRL,
			get32(_clk_base + CLK_DPI_CTRL) | CLK_CTRL_ENABLE);
}

/* ─── VIDEO_OUT_CFG and pads ─── */

static void rp1_dpi_vidout_setup(void) {
	/* VEC memories powered down; DPI and VDAC fed from DPI */
	put32(_cfg_base + VIDOUT_CFG_MEM_PD, VIDOUT_CFG_MEM_PD_VEC);
	put32(_cfg_base + VIDOUT_CFG_TEST_OVERRIDE, VIDOUT_TEST_OVR_VDAC);
	put32(_cfg_base + VIDOUT_CFG_SEL, 0);	/* DPI->pads, PCLK not inverted */
	put32(_cfg_base + VIDOUT_CFG_VDAC_CFG, 0);
	put32(_cfg_base + VIDOUT_CFG_INTE, 0);	/* polled driver, no IRQs */
}

/*
 * GPIO0..27 on funcsel 1 (ALTF1), per rp1.dtsi dpi pinctrl groups:
 *   mode 7 (24-bit): GPIO0-3 controls + D0..D23 on GPIO4..27
 *   mode 6 (DPI666): GPIO0-3 controls + B0..B5 on GPIO4..9,
 *     G0..G5 on GPIO12..17, R0..R5 on GPIO20..25; GPIO10/11/18/19 stay
 *     untouched (they carry e.g. the panel's touch I2C)
 */
static void rp1_dpi_pins_setup(uint32_t mode, int32_t bl_pin) {
	static const uint32_t pins_mode6[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
		12, 13, 14, 15, 16, 17,
		20, 21, 22, 23, 24, 25,
	};

	bcm2712_gpio_init();
	if (mode == 6) {
		for (uint32_t i = 0; i < sizeof(pins_mode6) / sizeof(pins_mode6[0]); i++) {
			bcm2712_gpio_pull(pins_mode6[i], GPIO_PULL_NONE);
			bcm2712_gpio_config(pins_mode6[i], GPIO_FUNC_ALTF1);
		}
	} else {
		for (uint32_t pin = 0; pin < 28; pin++) {
			bcm2712_gpio_pull(pin, GPIO_PULL_NONE);
			bcm2712_gpio_config(pin, GPIO_FUNC_ALTF1);
		}
	}

	/*
	 * Optional panel backlight pin (e.g. GPIO18 on the Waveshare 3.5inch
	 * DPI LCD, header pin 12). Only touch it when explicitly configured
	 * and never on a pin that carries DPI signals.
	 */
	if (bl_pin >= 0 && bl_pin < RP1_NUM_GPIOS) {
		bool is_dpi_pin = (mode == 6) ?
			(bl_pin <= 9 || (bl_pin >= 12 && bl_pin <= 17) ||
			 (bl_pin >= 20 && bl_pin <= 25)) :
			(bl_pin <= 27);
		if (!is_dpi_pin) {
			bcm2712_gpio_pull((uint32_t)bl_pin, GPIO_PULL_NONE);
			bcm2712_gpio_config((uint32_t)bl_pin, GPIO_FUNC_OUTPUT);
			bcm2712_gpio_write((uint32_t)bl_pin, true);
			slog("rp1-dpi: backlight pin %d driven high\n", bl_pin);
		} else {
			slog("rp1-dpi: bl_pin %d collides with DPI signals, ignored\n",
					bl_pin);
		}
	}
}

/* ─── DPI-DMA block ─── */

static int rp1_dpi_dma_wait_idle(void) {
	for (uint32_t waited = 0; waited < 100000U; waited += 100U) {
		if ((get32(_dpi_base + DPI_DMA_STATUS) &
				DPI_DMA_STATUS_BUSY_MASK) == 0)
			return 0;
		usleep(100);
	}
	slog("rp1-dpi: DMA won't idle status=%08x\n",
			get32(_dpi_base + DPI_DMA_STATUS));
	return -1;
}

/*
 * Configure the DPI-DMA timing and pixel format. Input is the in-memory
 * framebuffer format (XRGB8888 or RGB565), output is always 24-bit RGB888
 * on the pads. Field values are taken from rp1dpi_hw_setup()/
 * set_output_format() for MEDIA_BUS_FMT_RGB888_1X24.
 */
static void rp1_dpi_dma_setup(const bcm2712_dpi_timing_t *t,
		uint32_t dep, uint32_t w, uint32_t h) {
	uint32_t rgbsz, shift, imask, omask, ctrl;

	/* 24-bit pad output by default: R at 29:22, G at 19:12, B at 9:2 */
	omask = (0x3fcU << 0) | (0x3fcU << 10) | (0x3fcU << 20);

	if (dep == 16) {
		/* RGB565: R[15:11], G[10:5], B[4:0], scaled back up */
		rgbsz = (1U << 16) | (5U << 0) | (6U << 4) | (5U << 8);
		shift = (15U << 0) | (10U << 5) | (4U << 10);
		imask = (0x3e0U << 0) | (0x3f0U << 10) | (0x3e0U << 20);
	} else {
		/* XRGB8888: 8 bit per component at bit 23/15/7 */
		rgbsz = 3U << 16;
		shift = (23U << 0) | (15U << 5) | (7U << 10);
		imask = (0x3fcU << 0) | (0x3fcU << 10) | (0x3fcU << 20);
	}

	if (t->mode == 6) {
		/*
		 * DPI666 pads (legacy "mode 6"): 6 bits per component packed
		 * onto 18 data lines. rp1_dpi_hw.c's RGB666_1X24_CPADHI case.
		 */
		shift |= (27U << 15) | (17U << 20) | (7U << 25);
		omask = (0x3f0U << 0) | (0x3f0U << 10) | (0x3f0U << 20);
	} else {
		shift |= (29U << 15) | (19U << 20) | (9U << 25);
	}

	/* stop a previously armed engine before reprogramming */
	put32(_dpi_base + DPI_DMA_CONTROL,
			get32(_dpi_base + DPI_DMA_CONTROL) &
			~(DPI_DMA_CTRL_ARM | DPI_DMA_CTRL_AUTO_REPEAT));
	(void)rp1_dpi_dma_wait_idle();

	put32(_dpi_base + DPI_DMA_IMASK, imask);
	put32(_dpi_base + DPI_DMA_OMASK, omask);
	put32(_dpi_base + DPI_DMA_SHIFT, shift);
	put32(_dpi_base + DPI_DMA_RGBSZ, rgbsz);

	put32(_dpi_base + DPI_DMA_QOS,
			(0x0U << 0) | (0xbU << 4) | (0x2U << 8) |
			(0x8U << 12) | (0x7U << 16));

	put32(_dpi_base + DPI_DMA_VISIBLE_AREA,
			((h - 1U) << 0) | ((w - 1U) << 16));
	put32(_dpi_base + DPI_DMA_SYNC_WIDTH,
			((t->vsync - 1U) << 0) | ((t->hsync - 1U) << 16));
	/* "back porch" counts include the sync width */
	put32(_dpi_base + DPI_DMA_BACK_PORCH,
			((t->vbp + t->vsync - 1U) << 0) |
			((t->hbp + t->hsync - 1U) << 16));
	put32(_dpi_base + DPI_DMA_FRONT_PORCH,
			((t->vfp - 1U) << 0) | ((t->hfp - 1U) << 16));

	put32(_dpi_base + DPI_DMA_IRQ_FLAGS, 0xffffffffU);
	put32(_dpi_base + DPI_DMA_IRQ_EN, 0);

	ctrl = DPI_DMA_CTRL_ARM |
			DPI_DMA_CTRL_AUTO_REPEAT |
			(448U << DPI_DMA_CTRL_HIGH_WATER_LSB) |
			(t->hsync_pos ? 0U : DPI_DMA_CTRL_HSYNC_POL) |
			(t->vsync_pos ? 0U : DPI_DMA_CTRL_VSYNC_POL) |
			DPI_DMA_CTRL_HBP_EN | DPI_DMA_CTRL_HFP_EN |
			DPI_DMA_CTRL_VBP_EN | DPI_DMA_CTRL_VFP_EN |
			DPI_DMA_CTRL_HSYNC_EN | DPI_DMA_CTRL_VSYNC_EN;
	put32(_dpi_base + DPI_DMA_CONTROL, ctrl);
}

/*
 * Point the DMA engine at the framebuffer; with AUTO_REPEAT armed this
 * starts scanout immediately and keeps repeating it.
 */
static void rp1_dpi_dma_start(uint64_t bus_addr, uint32_t stride) {
	put32(_dpi_base + DPI_DMA_DMA_STRIDE, stride);
	put32(_dpi_base + DPI_DMA_DMA_ADDR_H, (uint32_t)(bus_addr >> 32));
	put32(_dpi_base + DPI_DMA_DMA_ADDR_L, (uint32_t)(bus_addr & 0xffffffffU));
}

/* ─── public entry point ─── */

static int rp1_dpi_make_timing(uint32_t w, uint32_t h, uint32_t dep,
		const bcm2712_dpi_timing_t *timing, bcm2712_dpi_timing_t *t) {
	if (timing != NULL && timing->pixel_clock_hz != 0) {
		if (timing->hfp == 0 || timing->hsync == 0 || timing->hbp == 0 ||
				timing->vfp == 0 || timing->vsync == 0 || timing->vbp == 0) {
			slog("rp1-dpi: incomplete explicit timing\n");
			return -1;
		}
		*t = *timing;
		return 0;
	}

	/* DPI panels have no EDID; CVT-RB gives sane 60Hz defaults */
	bcm2712_hdmi_mode_t cvt;
	if (bcm2712_native_hdmi_cvt_mode(w, h, dep, 60, &cvt) != 0) {
		slog("rp1-dpi: cvt timing gen failed %ux%ux%u\n", w, h, dep);
		return -1;
	}
	memset(t, 0, sizeof(*t));
	t->bl_pin = -1;
	t->pixel_clock_hz = cvt.pixel_clock_hz;
	t->hfp = cvt.hfp;
	t->hsync = cvt.hsync;
	t->hbp = cvt.hbp;
	t->vfp = cvt.vfp;
	t->vsync = cvt.vsync;
	t->vbp = cvt.vbp;
	t->hsync_pos = cvt.hsync_pos;
	t->vsync_pos = cvt.vsync_pos;
	return 0;
}

int bcm2712_rp1_dpi_init(const sys_info_t *sysinfo,
		uint32_t w, uint32_t h, uint32_t dep,
		const bcm2712_dpi_timing_t *timing, fbinfo_t *info) {
	bcm2712_dpi_timing_t t;
	uint32_t actual_fpix = 0;
	uint32_t bytes_per_pixel, pitch, size, alloc_size, page_size;
	ewokos_addr_t fb_vaddr, fb_phy;
	uint64_t bus_addr;

	if (sysinfo == NULL || info == NULL)
		return -1;
	if (dep != 16U && dep != 32U)
		return -1;
	if (w < 64U || h < 64U || w > 4096U || h > 2160U)
		return -1;

	if (rp1_dpi_make_timing(w, h, dep, timing, &t) != 0)
		return -1;

	if (rp1_dpi_map_windows() != 0)
		return -1;

	/* the DPI block is only reachable once the PCIe link to RP1 is up */
	if (bcm2712_rp1_init() != 0) {
		slog("rp1-dpi: RP1 not available\n");
		return -1;
	}

	if (rp1_dpi_clocks_setup(t.pixel_clock_hz, &actual_fpix) != 0)
		return -1;
	rp1_dpi_vidout_setup();
	if (t.mode != 6)
		t.mode = 7;	/* only mode 6 and 7 are wired up */
	rp1_dpi_pins_setup(t.mode, t.bl_pin);
	rp1_dpi_clocks_enable();

	bytes_per_pixel = dep / 8U;
	pitch = w * bytes_per_pixel;
	size = pitch * h;
	page_size = sysinfo->page_size == 0 ? 4096U : sysinfo->page_size;
	alloc_size = (size + page_size - 1U) & ~(page_size - 1U);

	fb_vaddr = dma_alloc(0, alloc_size);
	if (fb_vaddr == 0) {
		slog("rp1-dpi: dma alloc failed size=%u\n", alloc_size);
		return -1;
	}
	fb_phy = dma_phy_addr(0, fb_vaddr);
	if (fb_phy == 0) {
		slog("rp1-dpi: bad dma phy\n");
		dma_free(0, fb_vaddr);
		return -1;
	}
	memset((void *)(uintptr_t)fb_vaddr, 0, alloc_size);

	/* RP1 reads host RAM through the RC_BAR2 inbound window */
	bus_addr = fb_phy + 0x1000000000ULL;

	rp1_dpi_dma_setup(&t, dep, w, h);
	_dpi_bus_addr = bus_addr;
	_dpi_stride = pitch;
	rp1_dpi_dma_start(bus_addr, pitch);

	memset(info, 0, sizeof(*info));
	info->width = w;
	info->height = h;
	info->vwidth = w;
	info->vheight = h;
	info->depth = dep;
	info->pitch = pitch;
	info->pointer = fb_vaddr;
	info->phy_base = fb_phy;
	info->bus_base = bus_addr;
	info->size = size;
	info->size_max = alloc_size;
	info->xoffset = 0;
	info->yoffset = 0;
	info->dma_id = -1;

	slog("rp1-dpi: %ux%u@%u mode=%u pclk req=%u got=%u pitch=%u phy=%llx bus=%llx\n",
			w, h, dep, t.mode, t.pixel_clock_hz, actual_fpix, pitch,
			(unsigned long long)fb_phy, (unsigned long long)bus_addr);
	return 0;
}

/*
 * Periodic health check for callers that keep running after init. With
 * AUTO_REPEAT armed the engine scans out forever by itself; if STATUS shows
 * it idle anyway, something stopped it — log the block state and re-arm.
 * Also decodes the underflow flag and the PANICS counters (bit 1 of the
 * flags latches a DMA underflow; PANICS counts how often the FIFO hit the
 * panic levels), which is what to look at when the picture rolls.
 * Returns 0 while healthy, 1 after restarting a stopped engine, -1 if DPI
 * was never brought up.
 */
int bcm2712_rp1_dpi_check(void) {
	uint32_t status, flags, ctrl, panics;
	static uint32_t last_underflows;
	static uint32_t underflow_total;

	if (!_dpi_ready || _dpi_bus_addr == 0)
		return -1;

	status = get32(_dpi_base + DPI_DMA_STATUS);
	flags = get32(_dpi_base + DPI_DMA_IRQ_FLAGS);
	ctrl = get32(_dpi_base + DPI_DMA_CONTROL);
	panics = get32(_dpi_base + DPI_DMA_PANICS);

	if (flags & (1U << 1)) {	/* UNDERFLOW latches; write 1 to clear */
		underflow_total++;
		put32(_dpi_base + DPI_DMA_IRQ_FLAGS, (1U << 1));
	}
	slog("rp1-dpi: chk status=%08x flags=%08x ctrl=%08x panics=%08x undflw=%u\n",
			status, flags, ctrl, panics, underflow_total);

	if (underflow_total != last_underflows) {
		last_underflows = underflow_total;
		slog("rp1-dpi: UNDERFLOW detected (total=%u panics=%08x)\n",
				underflow_total, panics);
	}

	if ((status & DPI_DMA_STATUS_BUSY_MASK) != 0)
		return 0;

	flags = get32(_dpi_base + DPI_DMA_IRQ_FLAGS);
	slog("rp1-dpi: engine stopped status=%08x ctrl=%08x flags=%08x pll_cs=%08x\n",
			status, ctrl, flags, get32(_clk_base + PLL_VIDEO_CS));
	put32(_dpi_base + DPI_DMA_IRQ_FLAGS, flags);

	rp1_dpi_dma_start(_dpi_bus_addr, _dpi_stride);
	return 1;
}
