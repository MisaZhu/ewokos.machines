/*
 * RP1 MIPI DSI1 (display connector) output for BCM2712 (Raspberry Pi 5).
 *
 * The Pi5 DSI path lives inside the RP1 southbridge: an "ArgonDPI" DMA
 * block (0x130000 in the RP1 register file, same engine rp1_dpi uses)
 * scans the framebuffer out of host RAM and feeds it to a Synopsys DWC
 * MIPI DSI host (0x134000) with an SNPS D-PHY behind it; a small
 * RPI_MIPICFG block (0x138000) points the shared PHY at DSI rather than
 * CSI. Register programming follows raspberrypi/linux
 * drivers/gpu/drm/rp1/rp1-dsi/ (rp1_dsi_dsi.c, rp1_dsi_dma.c) and
 * drivers/clk/clk-rp1.c for the clock tree.
 *
 * Bring-up order mirrors the DRM atomic path: cfg clock + MIPICFG select
 * first, then host setup (PHY PLL, timings, command mode, lanes to LP-11),
 * then — with the lanes parked — the panel's I2C bring-up runs, and only
 * afterwards the DMA engine is armed and the host switched to video mode.
 *
 * The Waveshare DSI panels run with MIPI_DSI_MODE_VIDEO | VIDEO_HSE |
 * CLOCK_NON_CONTINUOUS, which on this host maps to sync-event video mode
 * with LP blanking and the HS clock gated during blanking.
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sysinfo.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <ewoksys/syscall.h>
#include <arch/bcm2712/mmio.h>
#include <arch/bcm2712/rp1.h>
#include <arch/bcm2712/rp1_dsi.h>

#define RP1_XOSP_HZ		50000000U

/* RP1 blocks used by DSI1, offsets inside the RP1 window */
#define RP1_CLOCKS_OFF		(PI5_RP1_WIN_OFF + 0x00018000)
#define RP1_DSI_DMA_OFF		(PI5_RP1_WIN_OFF + 0x00130000)
#define RP1_DSI_HOST_OFF	(PI5_RP1_WIN_OFF + 0x00134000)
#define RP1_MIPICFG_OFF		(PI5_RP1_WIN_OFF + 0x00138000)

/* ─── Synopsys DWC MIPI DSI host, relative to RP1_DSI_HOST_OFF ─── */

#define DSI_VERSION_CFG		0x000
#define DSI_PWR_UP		0x004
#define DSI_CLKMGR_CFG		0x008
#define DSI_DPI_VCID		0x00C
#define DSI_DPI_COLOR_CODING	0x010
#define DSI_DPI_CFG_POL		0x014
#define DSI_DPI_LP_CMD_TIM	0x018
#define DSI_PCKHDL_CFG		0x02C
#define DSI_GEN_VCID		0x030
#define DSI_MODE_CFG		0x034
#define DSI_VID_MODE_CFG	0x038
#define DSI_VID_PKT_SIZE	0x03C
#define DSI_VID_NUM_CHUNKS	0x040
#define DSI_VID_NULL_SIZE	0x044
#define DSI_VID_HSA_TIME	0x048
#define DSI_VID_HBP_TIME	0x04C
#define DSI_VID_HLINE_TIME	0x050
#define DSI_VID_VSA_LINES	0x054
#define DSI_VID_VBP_LINES	0x058
#define DSI_VID_VFP_LINES	0x05C
#define DSI_VID_VACTIVE_LINES	0x060
#define DSI_CMD_MODE_CFG	0x068
#define DSI_GEN_HDR		0x06C
#define DSI_GEN_PLD_DATA	0x070
#define DSI_CMD_PKT_STATUS	0x074
#define DSI_TO_CNT_CFG		0x078
#define DSI_BTA_TO_CNT		0x08C
#define DSI_LPCLK_CTRL		0x094
#define DSI_PHY_TMR_LPCLK_CFG	0x098
#define DSI_PHY_TMR_CFG		0x09C
#define DSI_PHYRSTZ		0x0A0
#define DSI_PHY_IF_CFG		0x0A4
#define DSI_PHY_STATUS		0x0B0
#define DSI_PHY_TST_CTRL0	0x0B4
#define DSI_PHY_TST_CTRL1	0x0B8

#define DSI_PCKHDL_EOTP_TX_EN	(1U << 0)
#define DSI_PCKHDL_BTA_EN	(1U << 2)

#define DSI_VID_MODE_LP_CMD_EN	(1U << 15)
#define DSI_VID_MODE_LP_HFP_EN	(1U << 13)
#define DSI_VID_MODE_LP_HBP_EN	(1U << 12)
#define DSI_VID_MODE_LP_VACT_EN	(1U << 11)
#define DSI_VID_MODE_LP_VFP_EN	(1U << 10)
#define DSI_VID_MODE_LP_VBP_EN	(1U << 9)
#define DSI_VID_MODE_LP_VSA_EN	(1U << 8)
#define DSI_VID_MODE_SYNC_EVENTS 1U
#define DSI_VID_MODE_BURST	2U

/* every command flavour transmitted in LP (rp1_dsi_dsi.c) */
#define DSI_CMD_MODE_ALL_LP	0x10f7f00U

#define DSI_PHYRSTZ_SHUTDOWNZ	(1U << 0)
#define DSI_PHYRSTZ_RSTZ	(1U << 1)
#define DSI_PHYRSTZ_ENABLECLK	(1U << 2)

#define DPHY_CTRL0_PHY_TESTCLK	(1U << 1)
#define DPHY_CTRL0_PHY_TESTCLR	(1U << 0)
#define DPHY_CTRL1_PHY_TESTEN	(1U << 16)
#define DPHY_CTRL1_PHY_TESTDOUT_LSB 8

/* D-PHY test-interface register addresses (dphy databook) */
#define DPHY_PLL_BIAS_OFFSET		0x10
#define DPHY_PLL_CHARGE_PUMP_OFFSET	0x11
#define DPHY_PLL_LPF_OFFSET		0x12
#define DPHY_PLL_INPUT_DIV_OFFSET	0x17
#define DPHY_PLL_LOOP_DIV_OFFSET	0x18
#define DPHY_PLL_DIV_CTRL_OFFSET	0x19
#define DPHY_HS_RX_CTRL_LANE0_OFFSET	0x44

/* ─── RPI_MIPICFG block, relative to RP1_MIPICFG_OFF ─── */

#define MIPICFG_CFG		0x04	/* SEL_CSI_DSI_N: 1 = CSI, 0 = DSI */
#define MIPICFG_DPHY_MONITOR	0x10
#define MIPICFG_INTE		0x2C

/* ─── DSI DMA block (same layout as rp1_dpi), relative to RP1_DSI_DMA_OFF ─── */

#define DPI_DMA_CONTROL		0x00
#define DPI_DMA_CTRL_ARM		(1U << 0)
#define DPI_DMA_CTRL_AUTO_REPEAT	(1U << 1)
#define DPI_DMA_CTRL_HIGH_WATER_LSB	3
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

/* ─── RP1 clock tree registers, relative to RP1_CLOCKS_OFF (clk-rp1.c) ─── */

#define CLK_MIPI1_CFG_CTRL	0x000d4
#define CLK_MIPI1_CFG_DIV_INT	0x000d8

#define VIDEO_CLK_MIPI1_DPI_CTRL	(0x4000 + 0x0030)
#define VIDEO_CLK_MIPI1_DPI_DIV_INT	(0x4000 + 0x0034)
#define VIDEO_CLK_MIPI1_DPI_DIV_FRAC	(0x4000 + 0x0038)
#define VIDEO_CLK_MIPI1_DPI_SEL	(0x4000 + 0x003c)

/* clk_mipi1_dpi aux parents (num_std_parents = 0): pll_sys, pll_video_sec,
 * pll_video, clksrc_mipi1_dsi_byteclk, gp0..gp3 */
#define MIPI1_DPI_AUXSRC_PLLSYS		0U
#define MIPI1_DPI_AUXSRC_DSI_BYTECLK	3U

/* PLL_SYS core + primary divider, used as the dpi parent when the pixel
 * stream is wider than the DSI link (bpp < 8*lanes) */
#define PLL_SYS_CS		0x08000
#define PLL_SYS_FBDIV_INT	0x08008
#define PLL_SYS_FBDIV_FRAC	0x0800c
#define PLL_SYS_PRIM		0x08010

#define CLK_CTRL_ENABLE		(1U << 11)
#define CLK_CTRL_AUXSRC_MASK	0x000003e0U
#define CLK_CTRL_AUXSRC_LSB	5
#define CLK_CTRL_SRC_LSB	0
#define CLK_CTRL_SRC_AUX	1U

/* Frequency limits from the linux driver */
#define RP1DSI_BYTE_CLK_MIN	10000000U
#define RP1DSI_BYTE_CLK_MAX	187500000U
#define RP1DSI_ESC_CLK_MAX	20000000U
#define RP1DSI_TO_CLK_DIV	0x50
#define RP1DSI_LPRRX_TO_VAL	0x40
#define RP1DSI_BTA_TO_VAL	0xd00

static ewokos_addr_t _clk_base;
static ewokos_addr_t _dma_base;
static ewokos_addr_t _host_base;
static ewokos_addr_t _cfg_base;
static int _dsi_ready;

static bcm2712_dsi_mode_t _dsi_mode;
static uint32_t _dsi_bpp = 24;	/* panels here always take RGB888 on the wire */
static uint32_t _dsi_byte_clock;
static uint32_t _dsi_hsfreq_index;
static uint64_t _dsi_bus_addr;
static uint32_t _dsi_stride;

static inline uint32_t host_read(uint32_t reg) {
	return get32(_host_base + reg);
}

static inline void host_write(uint32_t reg, uint32_t val) {
	put32(_host_base + reg, val);
}

static inline uint32_t dma_read(uint32_t reg) {
	return get32(_dma_base + reg);
}

static inline void dma_write(uint32_t reg, uint32_t val) {
	put32(_dma_base + reg, val);
}

/*
 * Same window setup as the other RP1 users (rp1_dpi, i2c, spi): map the
 * main MMIO window plus the RP1 register file.
 */
static int rp1_dsi_map_windows(void) {
	sys_info_t sysinfo;

	if (_dsi_ready)
		return 0;

	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
	_mmio_base = sysinfo.mmio.v_base;

	if (syscall3(SYS_MEM_MAP,
			(ewokos_addr_t)sysinfo.mmio.v_base,
			(ewokos_addr_t)sysinfo.mmio.phy_base,
			(ewokos_addr_t)sysinfo.mmio.size) != sysinfo.mmio.v_base) {
		slog("rp1-dsi: main mmio map failed\n");
		return -1;
	}
	if (syscall3(SYS_MEM_MAP,
			_mmio_base + PI5_RP1_WIN_OFF,
			(ewokos_addr_t)PI5_RP1_PHY,
			(ewokos_addr_t)PI5_RP1_WIN_SIZE) != _mmio_base + PI5_RP1_WIN_OFF) {
		slog("rp1-dsi: RP1 window map failed\n");
		return -1;
	}

	_clk_base = _mmio_base + RP1_CLOCKS_OFF;
	_dma_base = _mmio_base + RP1_DSI_DMA_OFF;
	_host_base = _mmio_base + RP1_DSI_HOST_OFF;
	_cfg_base = _mmio_base + RP1_MIPICFG_OFF;
	_dsi_ready = 1;
	return 0;
}

/* ─── SNPS D-PHY: test-interface transactions, PLL, hsfreqrange ─── */

/*
 * One D-PHY test-interface write (mipi dphy databook pg 101). APB writes
 * are slow enough that no extra delay is needed between the transitions.
 */
static void dphy_transaction(uint8_t test_code, uint8_t test_data) {
	host_write(DSI_PHY_TST_CTRL1, test_code | DPHY_CTRL1_PHY_TESTEN);
	host_write(DSI_PHY_TST_CTRL0, 0);
	(void)host_read(DSI_PHY_TST_CTRL1);
	host_write(DSI_PHY_TST_CTRL1, test_data);
	host_write(DSI_PHY_TST_CTRL0, DPHY_CTRL0_PHY_TESTCLK);
}

/*
 * Choose M/N for the D-PHY PLL (databook pg 77-78): fvco = m/n * refclk
 * with 40MHz >= refclk/n >= 5MHz, m even in [2,300], n in [1,100].
 * vco_freq is the lane bit rate (8x byte clock). Returns the actual VCO
 * frequency, or 0 when nothing lands within 1/64 of the target.
 */
static uint64_t dphy_get_div(uint32_t refclk, uint64_t vco_freq,
		uint32_t *ptr_m, uint32_t *ptr_n) {
	const uint32_t ref_divn_max = 40000000U;
	const uint32_t ref_divn_min = 5000000U;
	uint32_t n, best_n = 1, best_m = 2;
	uint64_t best_err = vco_freq;

	for (n = 1 + refclk / ref_divn_max;
			(uint64_t)n * ref_divn_min <= refclk && n < 100; ++n) {
		uint32_t half_m = (uint32_t)(((uint64_t)n * vco_freq + refclk) /
				(2U * refclk));

		if (half_m < 150) {
			uint64_t f = (uint64_t)(2 * half_m) * refclk / n;
			uint64_t err = (f > vco_freq) ? f - vco_freq : vco_freq - f;

			if (err < best_err) {
				best_n = n;
				best_m = 2 * half_m;
				best_err = err;
				if (err == 0)
					break;
			}
		}
	}

	if (64 * best_err >= vco_freq)
		return 0;

	*ptr_n = best_n;
	*ptr_m = best_m;
	return (uint64_t)best_m * refclk / best_n;
}

struct hsfreq_range {
	uint16_t mhz_max;
	uint8_t hsfreqrange;
	uint8_t clk_lp2hs;
	uint8_t clk_hs2lp;
	uint8_t data_lp2hs;
	uint8_t data_hs2lp;
};

/* D-PHY databook table A-3 (same table as the linux driver) */
static const struct hsfreq_range hsfreq_table[] = {
	{   89, 0x00, 32, 20, 26, 13 },
	{   99, 0x10, 35, 23, 28, 14 },
	{  109, 0x20, 32, 22, 26, 13 },
	{  129, 0x01, 31, 20, 27, 13 },
	{  139, 0x11, 33, 22, 26, 14 },
	{  149, 0x21, 33, 21, 26, 14 },
	{  169, 0x02, 32, 20, 27, 13 },
	{  179, 0x12, 36, 23, 30, 15 },
	{  199, 0x22, 40, 22, 33, 15 },
	{  219, 0x03, 40, 22, 33, 15 },
	{  239, 0x13, 44, 24, 36, 16 },
	{  249, 0x23, 48, 24, 38, 17 },
	{  269, 0x04, 48, 24, 38, 17 },
	{  299, 0x14, 50, 27, 41, 18 },
	{  329, 0x05, 56, 28, 45, 18 },
	{  359, 0x15, 59, 28, 48, 19 },
	{  399, 0x25, 61, 30, 50, 20 },
	{  449, 0x06, 67, 31, 55, 21 },
	{  499, 0x16, 73, 31, 59, 22 },
	{  549, 0x07, 79, 36, 63, 24 },
	{  599, 0x17, 83, 37, 68, 25 },
	{  649, 0x08, 90, 38, 73, 27 },
	{  699, 0x18, 95, 40, 77, 28 },
	{  749, 0x09, 102, 40, 84, 28 },
	{  799, 0x19, 106, 42, 87, 30 },
	{  849, 0x29, 113, 44, 93, 31 },
	{  899, 0x39, 118, 47, 98, 32 },
	{  949, 0x0a, 124, 47, 102, 34 },
	{  999, 0x1a, 130, 49, 107, 35 },
	{ 1049, 0x2a, 135, 51, 111, 37 },
	{ 1099, 0x3a, 139, 51, 114, 38 },
	{ 1149, 0x0b, 146, 54, 120, 40 },
	{ 1199, 0x1b, 153, 57, 125, 41 },
	{ 1249, 0x2b, 158, 58, 130, 42 },
	{ 1299, 0x3b, 163, 58, 135, 44 },
	{ 1349, 0x0c, 168, 60, 140, 45 },
	{ 1399, 0x1c, 172, 64, 144, 47 },
	{ 1449, 0x2c, 176, 65, 148, 48 },
	{ 1500, 0x3c, 181, 66, 153, 50 },
};

static void dphy_set_hsfreqrange(uint32_t freq_mhz) {
	unsigned int i;
	unsigned int last = sizeof(hsfreq_table) / sizeof(hsfreq_table[0]) - 1;

	if (freq_mhz < 80 || freq_mhz > 1500)
		slog("rp1-dsi: DPHY freq %u MHz out of range\n", freq_mhz);

	for (i = 0; i < last; i++) {
		if (freq_mhz <= hsfreq_table[i].mhz_max)
			break;
	}

	_dsi_hsfreq_index = i;
	dphy_transaction(DPHY_HS_RX_CTRL_LANE0_OFFSET,
			(uint8_t)(hsfreq_table[i].hsfreqrange << 1));
}

static uint32_t dphy_configure_pll(uint32_t refclk, uint32_t vco_freq) {
	uint32_t m = 0, n = 0;
	uint32_t actual = (uint32_t)dphy_get_div(refclk, vco_freq, &m, &n);

	if (actual != 0) {
		dphy_set_hsfreqrange(actual / 1000000U);
		/* program m,n from registers */
		dphy_transaction(DPHY_PLL_DIV_CTRL_OFFSET, 0x30);
		/* N (program N-1) */
		dphy_transaction(DPHY_PLL_INPUT_DIV_OFFSET, (uint8_t)(n - 1));
		/* M[8:5] */
		dphy_transaction(DPHY_PLL_LOOP_DIV_OFFSET,
				(uint8_t)(0x80 | ((m - 1) >> 5)));
		/* M[4:0] (program M-1) */
		dphy_transaction(DPHY_PLL_LOOP_DIV_OFFSET, (uint8_t)((m - 1) & 0x1F));
		slog("rp1-dsi: DPHY vco want %uHz got %uHz = %u * (%uHz / %u) hsfreqrange=0x%02x\n",
				vco_freq, actual, m, refclk, n,
				hsfreq_table[_dsi_hsfreq_index].hsfreqrange);
	} else {
		slog("rp1-dsi: error configuring DPHY PLL %uHz\n", vco_freq);
	}
	return actual;
}

/*
 * Reset the PHY and start its PLL. Returns the actual VCO frequency (the
 * caller derives the real byte clock from it); 0 on failure.
 */
static uint32_t dphy_init(uint32_t ref_freq, uint32_t vco_freq) {
	uint32_t actual_vco_freq;

	host_write(DSI_PHYRSTZ, 0);
	host_write(DSI_PHY_TST_CTRL0, DPHY_CTRL0_PHY_TESTCLK);
	host_write(DSI_PHY_TST_CTRL1, 0);
	host_write(DSI_PHY_TST_CTRL0,
			DPHY_CTRL0_PHY_TESTCLK | DPHY_CTRL0_PHY_TESTCLR);
	usleep(10);
	host_write(DSI_PHY_TST_CTRL0, DPHY_CTRL0_PHY_TESTCLK);
	usleep(10);
	/* DSI (not CSI2) mode: start the PLL */
	actual_vco_freq = dphy_configure_pll(ref_freq, vco_freq);
	usleep(10);
	host_write(DSI_PHYRSTZ, DSI_PHYRSTZ_SHUTDOWNZ);
	usleep(10);
	host_write(DSI_PHYRSTZ, DSI_PHYRSTZ_SHUTDOWNZ | DSI_PHYRSTZ_RSTZ);
	usleep(10);

	return actual_vco_freq;
}

/* ─── RP1 clocks: clk_mipi1_cfg and clk_mipi1_dpi ─── */

/*
 * clk_mipi1_cfg (ctrl 0xd4): the register-access clock of the whole MIPI1
 * block, sourced from xosc (its only aux parent). The dts keeps it at or
 * below 50MHz, i.e. a divide-by-1 of the 50MHz xosc.
 */
static void rp1_dsi_cfg_clock_enable(void) {
	put32(_clk_base + CLK_MIPI1_CFG_DIV_INT, 1);
	put32(_clk_base + CLK_MIPI1_CFG_CTRL,
			(get32(_clk_base + CLK_MIPI1_CFG_CTRL) &
			 ~(CLK_CTRL_AUXSRC_MASK | (1U << CLK_CTRL_SRC_LSB))) |
			(0U << CLK_CTRL_AUXSRC_LSB) |
			(CLK_CTRL_SRC_AUX << CLK_CTRL_SRC_LSB) |
			CLK_CTRL_ENABLE);
}

/* Rate of RP1's pll_sys, the fallback parent of clk_mipi1_dpi. */
static uint32_t rp1_pll_sys_rate(void) {
	uint32_t cs = get32(_clk_base + PLL_SYS_CS);
	uint32_t fbdiv_int = get32(_clk_base + PLL_SYS_FBDIV_INT);
	uint32_t fbdiv_frac = get32(_clk_base + PLL_SYS_FBDIV_FRAC);
	uint32_t prim = get32(_clk_base + PLL_SYS_PRIM);
	uint32_t refdiv = cs & 0x3fU;
	uint32_t div1 = (prim >> 16) & 0x7U;
	uint32_t div2 = (prim >> 12) & 0x7U;
	uint64_t vco;

	if (refdiv == 0)
		refdiv = 1;
	if (div1 == 0)
		div1 = 1;
	if (div2 == 0)
		div2 = 1;

	vco = ((uint64_t)RP1_XOSP_HZ *
			(((uint64_t)fbdiv_int << 24) + fbdiv_frac)) >> 24;
	return (uint32_t)(vco / refdiv / (div1 * div2));
}

/*
 * Point clk_mipi1_dpi at the DSI byte clock (preferred, keeps DPI and DSI
 * in an exact ratio) or at pll_sys when the in-memory stream is wider than
 * the link, and divide to the pixel rate — the 16.16 fixed-point divider
 * of this clock lands the rate exactly (rp1_dpi's plain clk_dpi has no
 * fractional register, this one does).
 */
static void rp1_dsi_dpi_clock_start(uint32_t byte_clock,
		uint32_t bpp, uint32_t lanes) {
	uint32_t dpi_rate = (4U * lanes * byte_clock) / (bpp >> 1);
	uint32_t auxsrc, parent_rate;
	uint64_t div_fp;

	if (bpp >= 8U * lanes) {
		auxsrc = MIPI1_DPI_AUXSRC_DSI_BYTECLK;
		parent_rate = byte_clock;
	} else {
		auxsrc = MIPI1_DPI_AUXSRC_PLLSYS;
		parent_rate = rp1_pll_sys_rate();
	}

	/* gate while reprogramming the divider/parent */
	put32(_clk_base + VIDEO_CLK_MIPI1_DPI_CTRL,
			get32(_clk_base + VIDEO_CLK_MIPI1_DPI_CTRL) & ~CLK_CTRL_ENABLE);

	div_fp = (((uint64_t)parent_rate << 16) + dpi_rate / 2) / dpi_rate;
	if (div_fp == 0)
		div_fp = 1ULL << 16;
	if (div_fp > (uint64_t)0xff << 16)
		div_fp = (uint64_t)0xff << 16;
	put32(_clk_base + VIDEO_CLK_MIPI1_DPI_DIV_INT, (uint32_t)(div_fp >> 16));
	put32(_clk_base + VIDEO_CLK_MIPI1_DPI_DIV_FRAC,
			(uint32_t)((div_fp & 0xffffU) << 16));

	put32(_clk_base + VIDEO_CLK_MIPI1_DPI_CTRL,
			(get32(_clk_base + VIDEO_CLK_MIPI1_DPI_CTRL) &
			 ~(CLK_CTRL_AUXSRC_MASK | (1U << CLK_CTRL_SRC_LSB))) |
			(auxsrc << CLK_CTRL_AUXSRC_LSB) |
			(CLK_CTRL_SRC_AUX << CLK_CTRL_SRC_LSB) |
			CLK_CTRL_ENABLE);

	slog("rp1-dsi: byte clock %u dpi clock target %u parent(%u) %u\n",
			byte_clock, dpi_rate, auxsrc, parent_rate);
}

/* ─── SNPS DSI host setup ─── */

/*
 * Full host + PHY programming for the mode, leaving the host in command
 * mode with the lanes at LP-11. Mirrors rp1dsi_dsi_setup() for the
 * Waveshare flag set (VIDEO | VIDEO_HSE | CLOCK_NON_CONTINUOUS, no LPM):
 * sync-event video mode with LP during all blanking intervals.
 * Returns 0 on success, a negative stage code on failure.
 */
static int rp1_dsi_host_setup(const bcm2712_dsi_mode_t *mode) {
	const uint32_t bpp = _dsi_bpp;
	const uint32_t lanes = mode->lanes;
	const uint32_t hdisplay = mode->width;
	const uint32_t vdisplay = mode->height;
	const uint32_t htotal = mode->width + mode->hfp + mode->hsw + mode->hbp;
	uint64_t byte_clock = ((uint64_t)bpp * mode->pixel_clock_hz) / (8U * lanes);
	uint32_t timeout, mask, clkdiv;
	uint32_t actual_vco;
	int cmdtim;

	if (byte_clock < RP1DSI_BYTE_CLK_MIN)
		byte_clock = RP1DSI_BYTE_CLK_MIN;
	if (byte_clock > RP1DSI_BYTE_CLK_MAX)
		byte_clock = RP1DSI_BYTE_CLK_MAX;
	_dsi_byte_clock = (uint32_t)byte_clock;

	host_write(DSI_PHY_IF_CFG, lanes - 1);
	host_write(DSI_DPI_CFG_POL, 0);
	host_write(DSI_GEN_VCID, 0);
	host_write(DSI_DPI_COLOR_CODING, 0x005);	/* RGB888 */

	/*
	 * Video-mode flavour (rp1dsi_dsi_setup).  vc4 drives every video
	 * panel — the official 7" protocol family included — in one fixed
	 * shape: LP_STOP_PERFRAME plus ST_END ("enables end events for
	 * HSYNC/VSYNC"), i.e. the lanes park once per frame in the
	 * vertical blanking and the sync packets are DSI sync events.  It
	 * ignores the SYNC_PULSE flag the 7" panel declares, so that
	 * family never actually ran sync pulses until the DWC host on RP1
	 * started honouring it — and the Waveshare clones of the 7"
	 * bridge only cope with the vc4 shape: per-line LP in the
	 * horizontal blanking (hfp=1 = 3 byte clocks) cannot even fit the
	 * ~30-UI HS->LP->HS turnaround.  Default (lp_hblank=0) therefore
	 * keeps the lanes HS across whole lines, parking them once per
	 * frame in the vertical blanking; sync_pulse then only picks the
	 * packet flavour (pulses vs events) on top.  lp_hblank=1 opts back
	 * into the official Pi5 recipe — every blanking interval LP, plus
	 * the dw-mipi-dsi HBP drop when the DPI clock is not an exact
	 * byte-clock multiple (8*lanes > bpp).
	 */
	mask = DSI_VID_MODE_LP_HFP_EN | DSI_VID_MODE_LP_HBP_EN |
			DSI_VID_MODE_LP_VACT_EN | DSI_VID_MODE_LP_VFP_EN |
			DSI_VID_MODE_LP_VBP_EN | DSI_VID_MODE_LP_VSA_EN;
	if (mode->sync_pulse)
		mask |= DSI_VID_MODE_LP_CMD_EN;
	else
		mask |= DSI_VID_MODE_SYNC_EVENTS;
	if (!mode->lp_hblank)
		mask &= ~(DSI_VID_MODE_LP_HFP_EN | DSI_VID_MODE_LP_HBP_EN |
				DSI_VID_MODE_LP_VACT_EN);
	else if (mode->sync_pulse && 8U * lanes > bpp)
		mask &= ~DSI_VID_MODE_LP_HBP_EN;
	host_write(DSI_VID_MODE_CFG, mask);
	/* LPM is the 7" family's own flag: commands always go out in LP */
	host_write(DSI_CMD_MODE_CFG, DSI_CMD_MODE_ALL_LP);
	/*
	 * vc4 transmits with DSI_CTRL_HSDT_EOT_DISABLE — no EoTp after
	 * HS packets.  The clone bridges of the 7" family have only ever
	 * seen that shape: a stray EoTp after every sync-event and pixel
	 * packet is charged to the data stream as a fixed per-line
	 * surplus, which desyncs the bridge into repeated bands.  Drop
	 * it; PCKHDL EOTP_TX_EN covers HS transmissions only, so the LP
	 * bridge-init writes (whose EoT vc4 keeps, LPDT not disabled)
	 * still carry theirs.
	 */
	host_write(DSI_PCKHDL_CFG, DSI_PCKHDL_BTA_EN);

	/* command mode first; video mode is switched on once the DMA runs */
	host_write(DSI_MODE_CFG, 1);

	/* timeouts and clock dividers */
	timeout = (bpp * htotal * vdisplay) / (7 * RP1DSI_TO_CLK_DIV * lanes);
	if (timeout > 0xFFFFu)
		timeout = 0;
	host_write(DSI_TO_CNT_CFG, (timeout << 16) | RP1DSI_LPRRX_TO_VAL);
	host_write(DSI_BTA_TO_CNT, RP1DSI_BTA_TO_VAL);
	clkdiv = 1 + (uint32_t)byte_clock / RP1DSI_ESC_CLK_MAX;
	if (clkdiv < 2)
		clkdiv = 2;
	host_write(DSI_CLKMGR_CFG, (RP1DSI_TO_CLK_DIV << 8) | clkdiv);

	/* video timings, in byte-clock units */
	host_write(DSI_VID_PKT_SIZE, hdisplay);
	host_write(DSI_VID_NUM_CHUNKS, 0);
	host_write(DSI_VID_NULL_SIZE, 0);
	host_write(DSI_VID_HSA_TIME, (bpp * mode->hsw) / (8 * lanes));
	host_write(DSI_VID_HBP_TIME, (bpp * mode->hbp) / (8 * lanes));
	host_write(DSI_VID_HLINE_TIME, (bpp * htotal) / (8 * lanes));
	host_write(DSI_VID_VSA_LINES, mode->vsw);
	host_write(DSI_VID_VBP_LINES, mode->vbp);
	host_write(DSI_VID_VFP_LINES, mode->vfp);
	host_write(DSI_VID_VACTIVE_LINES, vdisplay);

	/* init the D-PHY: VCO runs at the lane bit rate (8x byte clock) */
	actual_vco = dphy_init(RP1_XOSP_HZ, 8 * (uint32_t)byte_clock);
	if (actual_vco == 0)
		return -3;
	_dsi_byte_clock = actual_vco >> 3;

	host_write(DSI_PHY_TMR_LPCLK_CFG,
			((uint32_t)hsfreq_table[_dsi_hsfreq_index].clk_lp2hs << 0) |
			((uint32_t)hsfreq_table[_dsi_hsfreq_index].clk_hs2lp << 16));
	host_write(DSI_PHY_TMR_CFG,
			((uint32_t)hsfreq_table[_dsi_hsfreq_index].data_lp2hs << 0) |
			((uint32_t)hsfreq_table[_dsi_hsfreq_index].data_hs2lp << 16));

	/* LP bytes that fit into the horizontal blanking (databook 3.6.2.1) */
	cmdtim = (int)htotal;
	if (mode->sync_pulse)
		cmdtim -= (int)mode->hsw;
	cmdtim = ((int)bpp * cmdtim - 64) / (int)(8 * lanes);
	cmdtim -= hsfreq_table[_dsi_hsfreq_index].data_hs2lp;
	cmdtim -= hsfreq_table[_dsi_hsfreq_index].data_lp2hs;
	cmdtim = (cmdtim / (int)clkdiv) - 24;
	if (cmdtim < 0)
		cmdtim = 0;
	cmdtim >>= 4;
	host_write(DSI_DPI_LP_CMD_TIM, (uint32_t)cmdtim << 16);

	/* wait for PLL lock */
	for (timeout = (1U << 14); timeout != 0; --timeout) {
		usleep(20);
		if (host_read(DSI_PHY_STATUS) & (1U << 0))
			break;
	}
	if (timeout == 0) {
		slog("rp1-dsi: timeout waiting for DPHY PLL (status=%08x)\n",
				host_read(DSI_PHY_STATUS));
		return -4;
	}

	host_write(DSI_LPCLK_CTRL, mode->continuous_clock ? 0x1 : 0x3);
	host_write(DSI_PHY_TST_CTRL0, 0x2);
	host_write(DSI_PWR_UP, 0x1);

	/* the DPI engine may only run once the host is powered up */
	rp1_dsi_dpi_clock_start(_dsi_byte_clock, bpp, lanes);

	/* wait for all lanes to reach stopstate (LP-11) */
	mask = (1U << 4);
	if (lanes >= 2)
		mask |= (1U << 7);
	if (lanes >= 3)
		mask |= (1U << 9);
	if (lanes >= 4)
		mask |= (1U << 11);
	for (timeout = (1U << 10); timeout != 0; --timeout) {
		usleep(20);
		if ((host_read(DSI_PHY_STATUS) & mask) == mask)
			break;
	}
	if (timeout == 0) {
		slog("rp1-dsi: lanes never reached stopstate (want %08x got %08x)\n",
				mask, host_read(DSI_PHY_STATUS));
		return -5;
	}
	return 0;
}

/* ─── DSI DMA engine ─── */

static int rp1_dsi_dma_wait_idle(void) {
	for (uint32_t waited = 0; waited < 100000U; waited += 100U) {
		if ((dma_read(DPI_DMA_STATUS) & DPI_DMA_STATUS_BUSY_MASK) == 0)
			return 0;
		usleep(100);
	}
	slog("rp1-dsi: DMA won't idle status=%08x\n", dma_read(DPI_DMA_STATUS));
	return -1;
}

/*
 * Program the DMA engine (all registers except the base address, so the
 * scanout starts only once the caller writes the address) for a w x h
 * framebuffer of dep bits per pixel. Mirrors rp1dsi_dma_setup() with
 * output format RGB888 — the panel always takes RGB888 on the wire.
 */
static void rp1_dsi_dma_setup(uint32_t w, uint32_t h, uint32_t dep) {
	const bcm2712_dsi_mode_t *m = &_dsi_mode;
	uint32_t imask, shift, rgbsz, ctrl;

	/* stop a previously armed engine before reprogramming */
	dma_write(DPI_DMA_CONTROL,
			dma_read(DPI_DMA_CONTROL) &
			~(DPI_DMA_CTRL_ARM | DPI_DMA_CTRL_AUTO_REPEAT));
	(void)rp1_dsi_dma_wait_idle();

	dma_write(DPI_DMA_VISIBLE_AREA,
			((h - 1U) << 0) | ((w - 1U) << 16));
	dma_write(DPI_DMA_SYNC_WIDTH,
			((m->vsw - 1U) << 0) | ((m->hsw - 1U) << 16));
	/* in this engine "back porch" includes the sync width */
	dma_write(DPI_DMA_BACK_PORCH,
			((m->vbp + m->vsw - 1U) << 0) |
			((m->hbp + m->hsw - 1U) << 16));
	dma_write(DPI_DMA_FRONT_PORCH,
			((m->vfp - 1U) << 0) | ((m->hfp - 1U) << 16));

	if (dep == 16) {	/* in-memory RGB565 */
		imask = (0x3e0U << 0) | (0x3f0U << 10) | (0x3e0U << 20);
		shift = (15U << 0) | (10U << 5) | (4U << 10);
		rgbsz = (5U << 0) | (6U << 4) | (5U << 8) | (1U << 16);
	} else {		/* in-memory XRGB8888 */
		imask = (0x3fcU << 0) | (0x3fcU << 10) | (0x3fcU << 20);
		shift = (23U << 0) | (15U << 5) | (7U << 10);
		rgbsz = (3U << 16);
	}
	dma_write(DPI_DMA_IMASK, imask);
	/* out RGB888: MSB positions 23/15/7, 8 bits per channel */
	dma_write(DPI_DMA_OMASK,
			(0x3fcU << 0) | (0x3fcU << 10) | (0x3fcU << 20));
	dma_write(DPI_DMA_SHIFT, shift |
			(23U << 15) | (15U << 20) | (7U << 25));
	dma_write(DPI_DMA_RGBSZ, rgbsz);

	dma_write(DPI_DMA_QOS,
			(0x0U << 0) | (0xbU << 4) | (0x2U << 8) |
			(0x8U << 12) | (0x7U << 16));
	dma_write(DPI_DMA_IRQ_FLAGS, 0xffffffffU);
	/* no interrupt wired: mask the line, keep underflow latch visible */
	dma_write(DPI_DMA_IRQ_EN,
			(1U << 3) | (1U << 1) | (4095U << 16));

	if (dma_read(DPI_DMA_STATUS) & DPI_DMA_STATUS_BUSY_MASK)
		slog("rp1-dsi: DMA unexpectedly busy at start\n");

	ctrl = DPI_DMA_CTRL_ARM |
		DPI_DMA_CTRL_AUTO_REPEAT |
		(448U << DPI_DMA_CTRL_HIGH_WATER_LSB) |
		DPI_DMA_CTRL_HBP_EN | DPI_DMA_CTRL_HFP_EN |
		DPI_DMA_CTRL_VBP_EN | DPI_DMA_CTRL_VFP_EN |
		DPI_DMA_CTRL_HSYNC_EN | DPI_DMA_CTRL_VSYNC_EN;
	dma_write(DPI_DMA_CONTROL, ctrl);
}

static void rp1_dsi_dma_start(uint64_t bus_addr, uint32_t stride) {
	dma_write(DPI_DMA_DMA_STRIDE, stride);
	dma_write(DPI_DMA_DMA_ADDR_H, (uint32_t)(bus_addr >> 32));
	dma_write(DPI_DMA_DMA_ADDR_L, (uint32_t)(bus_addr & 0xffffffffU));
}

/* ─── public API ─── */

/*
 * Bring up clocks, D-PHY and the SNPS DSI host for the given mode. On
 * return the lanes sit at LP-11 with the host in command mode — run the
 * panel's I2C bring-up between this and bcm2712_rp1_dsi_video_start().
 */
int bcm2712_rp1_dsi_init(const bcm2712_dsi_mode_t *mode) {
	int rc;

	if (mode == NULL || mode->width == 0 || mode->height == 0 ||
			mode->pixel_clock_hz == 0 ||
			mode->lanes < 1 || mode->lanes > 4) {
		slog("rp1-dsi: bad mode\n");
		return -1;
	}

	rc = rp1_dsi_map_windows();
	if (rc != 0)
		return -1;
	if (bcm2712_rp1_init() != 0) {
		slog("rp1-dsi: RP1 init failed\n");
		return -1;
	}

	_dsi_mode = *mode;

	/* point the shared PHY at DSI and keep interrupts off (we poll) */
	put32(_cfg_base + MIPICFG_CFG, 0);
	put32(_cfg_base + MIPICFG_INTE, 0);

	rp1_dsi_cfg_clock_enable();

	rc = rp1_dsi_host_setup(mode);
	if (rc != 0)
		return rc;

	slog("rp1-dsi: host up %ux%u lanes=%u byteclk=%u hsfreq=%uMHz-entry\n",
			mode->width, mode->height, mode->lanes,
			_dsi_byte_clock, _dsi_hsfreq_index);
	return 0;
}

/*
 * Configure the DMA engine for the framebuffer, arm it (scanout starts
 * with the address write) and switch the host from command to video mode.
 */
int bcm2712_rp1_dsi_video_start(uint64_t bus_addr, uint32_t stride,
		uint32_t w, uint32_t h, uint32_t dep) {
	if (!_dsi_ready) {
		slog("rp1-dsi: video_start before init\n");
		return -1;
	}
	if (dep != 16 && dep != 32)
		return -1;

	rp1_dsi_dma_setup(w, h, dep);
	_dsi_bus_addr = bus_addr;
	_dsi_stride = stride;
	rp1_dsi_dma_start(bus_addr, stride);

	/* video mode: the DMA stream now drives the panel */
	host_write(DSI_MODE_CFG, 0);
	return 0;
}

/*
 * Same contract as bcm2712_rp1_dpi_check(): 0 = scanning out, 1 = engine
 * had stopped and was restarted (state logged), -1 = DSI not initialized.
 */
int bcm2712_rp1_dsi_check(void) {
	uint32_t status, flags, ctrl, panics;
	static uint32_t last_underflows;
	static uint32_t underflow_total;
	static uint32_t last_panics;

	if (!_dsi_ready || _dsi_bus_addr == 0)
		return -1;

	status = dma_read(DPI_DMA_STATUS);
	flags = dma_read(DPI_DMA_IRQ_FLAGS);
	ctrl = dma_read(DPI_DMA_CONTROL);
	panics = dma_read(DPI_DMA_PANICS);

	if (flags & (1U << 1)) {	/* UNDERFLOW latches; write 1 to clear */
		underflow_total++;
		dma_write(DPI_DMA_IRQ_FLAGS, (1U << 1));
	}
	if (underflow_total != last_underflows) {
		last_underflows = underflow_total;
		slog("rp1-dsi: UNDERFLOW detected (total=%u panics=%08x)\n",
				underflow_total, panics);
	} else if (panics != last_panics) {
		/* PANICS[15:0]/[31:16] are the two FIFO panic counters
		 * (cumulative); rising without a latched underflow still
		 * means the engine runs dry against the tiny 7" porches */
		slog("rp1-dsi: FIFO panics %08x -> %08x (underflows=%u)\n",
				last_panics, panics, underflow_total);
	}
	last_panics = panics;

	if ((status & DPI_DMA_STATUS_BUSY_MASK) != 0)
		return 0;

	flags = dma_read(DPI_DMA_IRQ_FLAGS);
	slog("rp1-dsi: engine stopped status=%08x ctrl=%08x flags=%08x phy=%08x\n",
			status, ctrl, flags, host_read(DSI_PHY_STATUS));
	dma_write(DPI_DMA_IRQ_FLAGS, flags);

	rp1_dsi_dma_start(_dsi_bus_addr, _dsi_stride);
	return 1;
}

/*
 * Send one generic packet in command mode — port of rp1dsi_dsi_send()
 * (rp1_dsi_dsi.c): wait for both FIFOs empty, select the LP/HS flavour
 * through VID_MODE_CFG/CMD_MODE_CFG, push the payload as 32-bit words
 * and the header (dt | vc<<6 | wc<<8), then wait for the FIFOs to drain
 * again. Must run while the host sits in command mode with the lanes
 * parked at LP-11 (i.e. after bcm2712_rp1_dsi_init(), before
 * bcm2712_rp1_dsi_video_start()) — the TC358762 bridge on the official
 * 7" protocol panels is initialized exactly this way.
 */
int bcm2712_rp1_dsi_cmd_write(uint8_t data_type, const uint8_t* data,
		int len, int lp) {
	uint32_t val, hdr;
	int i, spin;

	if (!_dsi_ready) {
		slog("rp1-dsi: cmd_write before init\n");
		return -1;
	}
	if (host_read(DSI_MODE_CFG) != 1) {
		slog("rp1-dsi: cmd_write outside command mode\n");
		return -1;
	}

	/* FIFOs idle = GEN_CMD_EMPTY(0) | GEN_PLD_W_EMPTY(2) set */
	for (spin = 0; spin < 256; ++spin) {
		if ((host_read(DSI_CMD_PKT_STATUS) & 0xfU) == 0x5U)
			break;
		usleep(100);
	}
	if ((host_read(DSI_CMD_PKT_STATUS) & 0xfU) != 0x5U) {
		slog("rp1-dsi: cmd FIFOs not idle (status=%08x)\n",
				host_read(DSI_CMD_PKT_STATUS));
		return -1;
	}

	val = host_read(DSI_VID_MODE_CFG);
	if (lp)
		val |= DSI_VID_MODE_LP_CMD_EN;
	else
		val &= ~DSI_VID_MODE_LP_CMD_EN;
	host_write(DSI_VID_MODE_CFG, val);
	host_write(DSI_CMD_MODE_CFG, lp ? DSI_CMD_MODE_ALL_LP : 0);
	(void)host_read(DSI_CMD_MODE_CFG);

	for (i = 0; i < len; i += 4) {
		val = data[i];
		if (i + 1 < len)
			val |= (uint32_t)data[i + 1] << 8;
		if (i + 2 < len)
			val |= (uint32_t)data[i + 2] << 16;
		if (i + 3 < len)
			val |= (uint32_t)data[i + 3] << 24;
		host_write(DSI_GEN_PLD_DATA, val);
	}
	/* GEN_HDR[7:0] = vc<<6 | dt, [23:8] = word count — the layout the
	 * rpi dw-mipi-dsi.c writes straight out of mipi_dsi_create_packet.
	 * Shifting dt here turns a 0x29 generic long write into a 0x24
	 * generic READ: the payload then never drains from the pld FIFO. */
	hdr = ((uint32_t)(len & 0xffffU) << 8) |
			(uint32_t)(data_type & 0x3fU);
	host_write(DSI_GEN_HDR, hdr);

	for (spin = 0; spin < 256; ++spin) {
		if ((host_read(DSI_CMD_PKT_STATUS) & 0xfU) == 0x5U)
			break;
		usleep(100);
	}
	if ((host_read(DSI_CMD_PKT_STATUS) & 0xfU) != 0x5U) {
		slog("rp1-dsi: cmd 0x%02x stuck (status=%08x)\n",
				data_type, host_read(DSI_CMD_PKT_STATUS));
		return -1;
	}
	return 0;
}

/*
 * Register snapshot of all three MIPI1 banks. Used on bring-up failure
 * and once on the live path: the repeated-band / tearing geometry bugs
 * hide in the effective video-timing values, so read back the VID_*
 * timings, the DMA geometry registers and the dpi clock SEL/DIV — a
 * mismatch between written and read-back values localises the fault.
 */
void bcm2712_rp1_dsi_dump(void) {
	if (!_dsi_ready) {
		slog("rp1-dsi: dump before init\n");
		return;
	}
	slog("rp1-dsi dump host: ver=%08x pwr=%08x mode=%08x vidmode=%08x clkmgr=%08x\n",
			host_read(DSI_VERSION_CFG), host_read(DSI_PWR_UP),
			host_read(DSI_MODE_CFG), host_read(DSI_VID_MODE_CFG),
			host_read(DSI_CLKMGR_CFG));
	slog("rp1-dsi dump host: phy_status=%08x phyrstz=%08x lpclk=%08x cmdpkt=%08x pckhdl=%08x\n",
			host_read(DSI_PHY_STATUS), host_read(DSI_PHYRSTZ),
			host_read(DSI_LPCLK_CTRL), host_read(DSI_CMD_PKT_STATUS),
			host_read(DSI_PCKHDL_CFG));
	slog("rp1-dsi dump vid: pkt=%08x hsa=%08x hbp=%08x hline=%08x\n",
			host_read(DSI_VID_PKT_SIZE), host_read(DSI_VID_HSA_TIME),
			host_read(DSI_VID_HBP_TIME), host_read(DSI_VID_HLINE_TIME));
	slog("rp1-dsi dump vid: vsa=%08x vbp=%08x vfp=%08x vact=%08x\n",
			host_read(DSI_VID_VSA_LINES), host_read(DSI_VID_VBP_LINES),
			host_read(DSI_VID_VFP_LINES), host_read(DSI_VID_VACTIVE_LINES));
	slog("rp1-dsi dump dma: ctrl=%08x status=%08x flags=%08x addr=%08x%08x stride=%08x\n",
			dma_read(DPI_DMA_CONTROL), dma_read(DPI_DMA_STATUS),
			dma_read(DPI_DMA_IRQ_FLAGS), dma_read(DPI_DMA_DMA_ADDR_H),
			dma_read(DPI_DMA_DMA_ADDR_L), dma_read(DPI_DMA_DMA_STRIDE));
	slog("rp1-dsi dump dma: visible=%08x sync=%08x bp=%08x fp=%08x rgbsz=%08x panics=%08x\n",
			dma_read(DPI_DMA_VISIBLE_AREA), dma_read(DPI_DMA_SYNC_WIDTH),
			dma_read(DPI_DMA_BACK_PORCH), dma_read(DPI_DMA_FRONT_PORCH),
			dma_read(DPI_DMA_RGBSZ), dma_read(DPI_DMA_PANICS));
	slog("rp1-dsi dump cfg: cfg=%08x mon=%08x clk_ctrl=%08x dpi_ctrl=%08x\n",
			get32(_cfg_base + MIPICFG_CFG),
			get32(_cfg_base + MIPICFG_DPHY_MONITOR),
			get32(_clk_base + CLK_MIPI1_CFG_CTRL),
			get32(_clk_base + VIDEO_CLK_MIPI1_DPI_CTRL));
	/* SEL is the glitchless-mux selection bitmap: bit N = parent N of
	 * clk_mipi1_dpi (aux 3 = the DSI byte clock); DIV is 16.16 */
	slog("rp1-dsi dump clk: dpi_div=%08x.%08x dpi_sel=%08x cfg_div=%08x\n",
			get32(_clk_base + VIDEO_CLK_MIPI1_DPI_DIV_INT),
			get32(_clk_base + VIDEO_CLK_MIPI1_DPI_DIV_FRAC),
			get32(_clk_base + VIDEO_CLK_MIPI1_DPI_SEL),
			get32(_clk_base + CLK_MIPI1_CFG_DIV_INT));
}
