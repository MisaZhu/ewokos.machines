/*
 * adreno_gpucc.c - SC8280XP GPU clock controller (GPUCC) bring-up.
 *
 * EwokOS has no Linux clk framework, so the GMU/GPU leaf clocks that Linux
 * enables through gpucc-sc8280xp.c must be programmed directly here.  Every
 * register offset, enable mask and RCG config value below is taken verbatim
 * from (or derived from) upstream drivers/clk/qcom/gpucc-sc8280xp.c and
 * clk-rcg2.c (v6.6):
 *
 *   - RCG CFG encoding follows clk-rcg2.c __clk_rcg2_configure():
 *       cfg = (pre_div << CFG_SRC_DIV_SHIFT[0]) | (src_cfg << CFG_SRC_SEL_SHIFT[8])
 *     with pre_div = 2*div - 1 (the F() macro in clk-rcg.h) and src_cfg the
 *     parent_map entry.  mnd_width == 0 for both RCGs, so no M/N/D and no
 *     dual-edge mode is programmed.
 *       gpu_cc_gmu_clk_src (cmd 0x1120): 200 MHz = GPLL0_DIV_CLK_SRC / 1.5
 *         -> pre_div = 2*1.5-1 = 2, parent_map_0[GCC_GPU_GPLL0_DIV_CLK_SRC]=6
 *         -> CFG(0x1124) = (6 << 8) | 2 = 0x602
 *       gpu_cc_hub_clk_src (cmd 0x117c): 200 MHz = GPLL0_CLK_SRC / 3
 *         -> pre_div = 2*3-1 = 5, parent_map_1[GCC_GPU_GPLL0_CLK_SRC]=5
 *         -> CFG(0x1180) = (5 << 8) | 5 = 0x505
 *
 *   - Branch clocks are enabled by setting BIT(0) of their enable_reg and
 *     polling BIT(31) (HW_OFF) of halt_reg to clear (clk-branch.c).
 *
 * HARD DEPENDENCIES (not satisfiable from this file, documented per the
 * evidence requirement - do NOT assume they are done):
 *   1. GCC must already supply gpu_cc gpll0 (600 MHz) and gpll0_div (300 MHz);
 *      these are the RCG parents.  On SC8280XP the bootloader/GCC leaves them
 *      running, but EwokOS does not program GCC.
 *   2. gpu_cc_pll0/pll1 (drivers/clk/qcom/clk-alpha-pll.c lucid 5lpe) are NOT
 *      configured here.  They feed the 500 MHz gmu option and the GPU core
 *      (gfx) clock.  The GMU brings the GX/core domain up through RPMh GFX
 *      votes (a6xx_gmu_power_config), so the 200 MHz GPLL0-derived GMU clock
 *      path is what this file provides.
 *   3. The CX/GX GDSCs are VOTABLE / hw-controlled (cx_gdsc VOTABLE,
 *      gx_gdsc.power_on = gdsc_gx_do_nothing_enable); the GMU/RPMh owns them,
 *      so this file must NOT toggle the GDSCR bits directly.
 *
 * NOT hardware-validated on SC8280XP.
 */
#include <adreno/adreno_drv.h>
#include <adreno/adreno_reg.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>

/* clk-rcg2.c register layout, relative to cmd_rcgr. */
#define RCG_CMD_REG        0x0
#define RCG_CFG_REG        0x4
#define CMD_UPDATE         (1u << 0)
#define CMD_ROOT_EN        (1u << 1)
#define CMD_ROOT_OFF       (1u << 31)

/* clk-branch.c: enable mask + halt (HW_OFF) status bit. */
#define BRANCH_CLK_EN      (1u << 0)
#define BRANCH_HW_OFF      (1u << 31)

/* gpucc-sc8280xp.c register offsets (byte offsets in the GPUCC regmap). */
#define GPUCC_GMU_CLK_SRC_CMD   0x1120u
#define GPUCC_GMU_CLK_SRC_CFG   0x1124u
#define GPUCC_HUB_CLK_SRC_CMD   0x117cu
#define GPUCC_HUB_CLK_SRC_CFG   0x1180u

#define GPUCC_AHB_CLK           0x1078u
#define GPUCC_CX_GMU_CLK        0x1098u
#define GPUCC_GX_GMU_CLK        0x1064u
#define GPUCC_HUB_AON_CLK       0x1178u
#define GPUCC_HUB_CX_INT_CLK    0x1204u
#define GPUCC_CXO_AON_CLK       0x1004u
#define GPUCC_HLOS1_VOTE_SMMU   0x5000u

/* probe() "keep always-ON": GPU_CC_CB_CLK (0x1170), GPU_CC_CXO_CLK (0x109c). */
#define GPUCC_CB_CLK_ALWAYS_ON  0x1170u
#define GPUCC_CXO_CLK_ALWAYS_ON 0x109cu

/* The GPUCC accessors are word-indexed; gpucc offsets are byte offsets. */
static void cc_write(uint32_t byteoff, uint32_t val) {
	adreno_gpucc_write(byteoff >> 2, val);
}
static uint32_t cc_read(uint32_t byteoff) {
	return adreno_gpucc_read(byteoff >> 2);
}

static void cc_set_bits(uint32_t byteoff, uint32_t mask, uint32_t val) {
	uint32_t r = cc_read(byteoff);
	r = (r & ~mask) | (val & mask);
	cc_write(byteoff, r);
}

/* Configure an RCG (write CFG, latch with CMD_UPDATE, then ROOT_EN). */
static int rcg_configure_enable(uint32_t cmd, uint32_t cfg, uint32_t cfgval) {
	int i;
	cc_write(cfg, cfgval);
	cc_set_bits(cmd, CMD_UPDATE, CMD_UPDATE);
	for (i = 0; i < 100000; i++) {
		if (!(cc_read(cmd) & CMD_UPDATE))
			break;
		proc_nsleep_precise(1000);
	}
	if (cc_read(cmd) & CMD_UPDATE)
		return -1;
	cc_set_bits(cmd, CMD_ROOT_EN, CMD_ROOT_EN);
	return 0;
}

/* Enable a branch clock and wait for its halt (HW_OFF) bit to clear. */
static int branch_enable(uint32_t reg) {
	int i;
	cc_set_bits(reg, BRANCH_CLK_EN, BRANCH_CLK_EN);
	for (i = 0; i < 100000; i++) {
		if (!(cc_read(reg) & BRANCH_HW_OFF))
			return 0;
		proc_nsleep_precise(1000);
	}
	/* Some branches are voted/aon and never report a stable halt bit;
	   treat a stuck HW_OFF as non-fatal but log it. */
	klog("adreno_gpucc: branch 0x%x HW_OFF stuck (0x%x)\n", reg, cc_read(reg));
	return 0;
}

int adreno_gpucc_init(void) {
	/* Root clock generators first: GMU 200 MHz (GPLL0_DIV/1.5) and hub
	   200 MHz (GPLL0/3).  Values derived in the header comment. */
	if (rcg_configure_enable(GPUCC_GMU_CLK_SRC_CMD, GPUCC_GMU_CLK_SRC_CFG,
			(6u << 8) | 2u) != 0) {
		klog("adreno_gpucc: gmu_clk_src failed to latch\n");
		return -1;
	}
	if (rcg_configure_enable(GPUCC_HUB_CLK_SRC_CMD, GPUCC_HUB_CLK_SRC_CFG,
			(5u << 8) | 5u) != 0) {
		klog("adreno_gpucc: hub_clk_src failed to latch\n");
		return -2;
	}

	/* Leaf branches feeding the GMU / hub / SMMU. */
	branch_enable(GPUCC_CXO_AON_CLK);
	branch_enable(GPUCC_AHB_CLK);
	branch_enable(GPUCC_HUB_AON_CLK);
	branch_enable(GPUCC_HUB_CX_INT_CLK);
	branch_enable(GPUCC_CX_GMU_CLK);
	branch_enable(GPUCC_GX_GMU_CLK);
	branch_enable(GPUCC_HLOS1_VOTE_SMMU);

	/* Keep the CB and CXO clocks always on (gpucc probe()). */
	cc_set_bits(GPUCC_CB_CLK_ALWAYS_ON, BRANCH_CLK_EN, BRANCH_CLK_EN);
	cc_set_bits(GPUCC_CXO_CLK_ALWAYS_ON, BRANCH_CLK_EN, BRANCH_CLK_EN);

	adreno_mb();
	klog("adreno_gpucc: GMU/hub clocks enabled (200 MHz)\n");
	return 0;
}
