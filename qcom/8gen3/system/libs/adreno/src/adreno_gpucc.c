/*
 * adreno_gpucc.c - SC8280XP GPU clock controller + GCC GPU branch bring-up.
 *
 * EwokOS has no Linux clk framework, so every clock that Linux enables
 * through the parent chain of gpucc-sc8280xp.c has to be programmed
 * directly here:
 *
 *   GCC (global clock controller, 0x0010_0000)
 *     gcc_gpu_gpll0_clk_src       0x052000 BIT(15)  parent of hub_clk_src
 *     gcc_gpu_gpll0_div_clk_src   0x052000 BIT(16)  parent of gmu_clk_src
 *     gcc_ddrss_gpu_axi_clk       0x07115c BIT(0)   GMU/DDR AXI path
 *     gcc_gpu_memnoc_gfx_clk      0x071010 BIT(0)   GMU NoC path
 *   (drivers/clk/qcom/gcc-sc8280xp.c; the GPUCC RCGs list GCC_GPU_GPLL0*
 *    as their DT parents, and the GMU node's clock list contains the two
 *    NoC branches.)
 *
 *   GPUCC (0x03d9_0000)
 *     cx_gdsc                     0x106c SW_COLLAPSE / 0x1540 PWR_ON
 *     gpu_cc_gmu_clk_src (RCG)    0x1120, parent GPLL0_DIV, /1.5 = 200 MHz
 *     gpu_cc_hub_clk_src (RCG)    0x117c, parent GPLL0, /3       = 200 MHz
 *     leaf branches               CXO, AHB, HUB_AON, HUB_CX_INT,
 *                                 CX_GMU, HLOS1_VOTE_SMMU
 *     late leaf                   GX_GMU (only after the GMU has raised the
 *                                 GX rail; see adreno_gpucc_gx_clk_enable)
 *
 * RCG CFG encoding follows clk-rcg2.c __clk_rcg2_configure():
 *   cfg = (pre_div << CFG_SRC_DIV_SHIFT[0]) | (src << CFG_SRC_SEL_SHIFT[8])
 * with pre_div = 2*div - 1 (F() in clk-rcg.h).  For 200 MHz:
 *   gmu_clk_src: GPLL0_DIV (300 MHz) / 1.5 -> src 6, pre_div 2 -> 0x602
 *   hub_clk_src: GPLL0     (600 MHz) / 3   -> src 5, pre_div 5 -> 0x505
 *
 * Branch enable = BIT(0) of the enable/halt register.  A branch counts as
 * up when CBCR_CLK_OFF (BIT 31) is clear OR CBCR_NOC_FSM_STATUS (bits
 * 30:28) reads FSM_STATUS_ON, exactly like clk-branch.c
 * clk_branch2_check_halt().  Several GPUCC branches (CXO, SMMU vote)
 * are voted/always-on and may keep BIT(31) set; that is logged, not fatal.
 *
 * Ordering matches Linux's a6xx_gmu_resume(): the GCC parents first, then
 * the CX power domain (genpd `cx_gdsc`, the gmu node's "cx" power domain),
 * then the RCGs and leaf clocks.  GX_GMU cannot start before the GMU has
 * asked RPMh/AOP for the GX rail (gx_gdsc is `gdsc_gx_do_nothing_enable` in
 * Linux, i.e. the GMU owns it), so it is enabled later, from adreno_gmu.c,
 * after the GPU_SET OOB handshake and the GX power-status poll.
 *
 * HARD DEPENDENCIES that remain: gpu_cc_pll1 (GPU core/gfx clock) is not
 * programmed here; the GPU core rate comes from the GMU/RPMh votes.
 *
 * NOT hardware-validated on SC8280XP (see the bring-up log in README.md).
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

/* clk-branch.c: enable mask + halt status bits. */
#define BRANCH_CLK_EN      (1u << 0)
#define BRANCH_CLK_OFF     (1u << 31)
#define BRANCH_FSM_SHIFT   28
#define BRANCH_FSM_MASK    0x7u
#define BRANCH_FSM_ON      2u      /* FSM_STATUS_ON */

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
#define GPUCC_CXO_CLK           0x109cu
#define GPUCC_HLOS1_VOTE_SMMU   0x5000u

/* probe() keeps GPU_CC_CB_CLK on; GPU_CC_CXO_CLK is a required GMU clock. */
#define GPUCC_CB_CLK_ALWAYS_ON  0x1170u

/* cx_gdsc (gpucc-sc8280xp.c): gdscr + gds_hw_ctrl.  gx_gdsc (0x100c) is
   deliberately NOT touched - its power_on is gdsc_gx_do_nothing_enable and
   the GMU raises the GX rail through RPMh. */
#define GPUCC_CX_GDSCR          0x106cu
#define GPUCC_CX_GDSCR_CFG      0x1070u
#define GPUCC_CX_GDSC_HW_CTRL   0x1540u
/* gdsc.c: the GDSCR retention bits Linux sets in gdsc_enable(). */
#define GDSC_RETAIN_MEM         (1u << 14)
#define GDSC_RETAIN_PERIPH      (1u << 13)
#define GDSC_RETAIN_FF          (1u << 11)
#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_PWR_ON             (1u << 31)
#define GDSC_RETAIN_FF_ENABLE   (1u << 11)

/* GCC GPU branch clocks (byte offsets in the GCC regmap). */
#define GCC_GPU_GPLL0_BRANCH    0x052000u
#define GCC_GPU_GPLL0_EN        (1u << 15)
#define GCC_GPU_GPLL0_DIV_EN    (1u << 16)
#define GCC_DDRSS_GPU_AXI_CLK   0x07115cu
#define GCC_GPU_MEMNOC_GFX_CLK  0x071010u
/* The GPU SMMU node lists snoc_dvm_gfx for its DVM path.  Linux enables it
   through the arm-smmu-qcom clock list; EwokOS has no such driver. */
#define GCC_GPU_SNOC_DVM_GFX_CLK 0x071020u

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

/* clk_branch2_check_halt(): on == CLK_OFF clear, or the NoC FSM reports ON. */
static int branch_is_on(uint32_t val) {
	if (!(val & BRANCH_CLK_OFF))
		return 1;
	return ((val >> BRANCH_FSM_SHIFT) & BRANCH_FSM_MASK) == BRANCH_FSM_ON;
}

/* ------------------------------------------------------------------ */
/* GCC: the GPU branch clocks that parent the GPUCC RCGs.               */
/* ------------------------------------------------------------------ */
static void gcc_set(uint32_t byteoff, uint32_t mask) {
	adreno_gcc_write(byteoff, adreno_gcc_read(byteoff) | mask);
}

static int gcc_gpu_clocks_enable(void) {
	uint32_t before = adreno_gcc_read(GCC_GPU_GPLL0_BRANCH);
	uint32_t after;

	/* gcc_gpu_gpll0_clk_src / gcc_gpu_gpll0_div_clk_src: BRANCH_HALT_DELAY,
	   no status bit - enable and give the RCG parent 10 us (Linux udelay). */
	gcc_set(GCC_GPU_GPLL0_BRANCH, GCC_GPU_GPLL0_EN | GCC_GPU_GPLL0_DIV_EN);

	/* gcc_ddrss_gpu_axi_clk: HALT_SKIP (hwcg bit 1 in the same register). */
	gcc_set(GCC_DDRSS_GPU_AXI_CLK, BRANCH_CLK_EN);
	/* gcc_gpu_memnoc_gfx_clk: HALT_VOTED + hwcg. */
	gcc_set(GCC_GPU_MEMNOC_GFX_CLK, BRANCH_CLK_EN);
	/* SMMU DVM path (see the iommu node's clock list). */
	gcc_set(GCC_GPU_SNOC_DVM_GFX_CLK, BRANCH_CLK_EN);

	adreno_mb();
	proc_nsleep_precise(10000);

	after = adreno_gcc_read(GCC_GPU_GPLL0_BRANCH);
	klog("adreno_gpucc: gcc gpu branches %x -> %x (axi=%x memnoc=%x dvm=%x)\n",
			before, after,
			adreno_gcc_read(GCC_DDRSS_GPU_AXI_CLK),
			adreno_gcc_read(GCC_GPU_MEMNOC_GFX_CLK),
			adreno_gcc_read(GCC_GPU_SNOC_DVM_GFX_CLK));

	if (!(after & GCC_GPU_GPLL0_EN))
		return -1;
	return (after & GCC_GPU_GPLL0_DIV_EN) ? 0 : -2;
}

/* ------------------------------------------------------------------ */
/* CX power domain (genpd `cx_gdsc` in Linux, gmu's "cx" power domain). */
/* ------------------------------------------------------------------ */
static int cx_gdsc_power_on(void) {
	uint32_t v;
	int i;

	/* Mirror Linux's gdsc_enable() for cx_gdsc (VOTABLE |
	   RETAIN_FF_ENABLE): always clear SW_COLLAPSE, force the memory and
	   peripheral retention bits on, then enable the flop retention.
	   Skipping this when PWR_ON was already set left RETAIN_MEM and
	   RETAIN_FF clear (GDSCR=0xf8222000), which can leave the GMU's
	   SRAM/flops unpowered across the CM3 release. */
	cc_set_bits(GPUCC_CX_GDSCR, GDSC_SW_COLLAPSE, 0);
	proc_nsleep_precise(1000);
	for (i = 0; i < 1000; i++) {
		if (cc_read(GPUCC_CX_GDSC_HW_CTRL) & GDSC_PWR_ON)
			break;
		proc_nsleep_precise(1000);
	}
	v = cc_read(GPUCC_CX_GDSC_HW_CTRL);
	if (!(v & GDSC_PWR_ON)) {
		klog("adreno_gpucc: cx_gdsc did not power on (%x)\n", v);
		return -1;
	}
	cc_set_bits(GPUCC_CX_GDSCR, GDSC_RETAIN_MEM | GDSC_RETAIN_PERIPH,
	            GDSC_RETAIN_MEM | GDSC_RETAIN_PERIPH);
	proc_nsleep_precise(1000);
	cc_set_bits(GPUCC_CX_GDSCR, GDSC_RETAIN_FF, GDSC_RETAIN_FF);
	klog("adreno_gpucc: cx_gdsc on, gdscr=%x hw=%x\n",
	     cc_read(GPUCC_CX_GDSCR), cc_read(GPUCC_CX_GDSC_HW_CTRL));
	return 0;
}

/* Configure an RCG: force the root on (CMD_ROOT_EN, as
   clk_rcg2_set_force_enable does), wait for CMD_ROOT_OFF to clear, write the
   CFG value and latch it with CMD_UPDATE.  ROOT_EN is left asserted: EwokOS
   has no clk framework to gate the root again and every user of these RCGs
   is enabled for the lifetime of the GPU driver. */
static int rcg_configure_enable(uint32_t cmd, uint32_t cfg, uint32_t cfgval) {
	int i;
	cc_set_bits(cmd, CMD_ROOT_EN, CMD_ROOT_EN);
	for (i = 0; i < 1000; i++) {
		if (!(cc_read(cmd) & CMD_ROOT_OFF))
			break;
		proc_nsleep_precise(1000);
	}
	if (cc_read(cmd) & CMD_ROOT_OFF) {
		klog("adreno_gpucc: rcg %x root stays off (%x)\n",
				cmd, cc_read(cmd));
		return -1;
	}
	cc_write(cfg, cfgval);
	cc_set_bits(cmd, CMD_UPDATE, CMD_UPDATE);
	for (i = 0; i < 100000; i++) {
		if (!(cc_read(cmd) & CMD_UPDATE))
			break;
		proc_nsleep_precise(1000);
	}
	if (cc_read(cmd) & CMD_UPDATE)
		return -1;
	return 0;
}

/* Enable a branch clock and wait for it to report on (Linux semantics). */
static int branch_enable(uint32_t reg) {
	int i;
	cc_set_bits(reg, BRANCH_CLK_EN, BRANCH_CLK_EN);
	for (i = 0; i < 1000; i++) {
		if (branch_is_on(cc_read(reg)))
			return 0;
		proc_nsleep_precise(1000);
	}
	klog("adreno_gpucc: branch 0x%x not on (0x%x)\n", reg, cc_read(reg));
	return -1;
}

int adreno_gpucc_init(void) {
	int ret;

	/* 1. GCC parents of the two GPUCC RCGs, plus the GMU NoC branches.
	      Must come first: without gcc_gpu_gpll0_div_clk_src the gmu_clk_src
	      RCG produces no clock and both GMU branches stay halted. */
	ret = gcc_gpu_clocks_enable();
	if (ret != 0) {
		klog("adreno_gpucc: required GPLL0 parents unavailable (%d)\n", ret);
		return -1;
	}

	/* 2. The CX power domain must be up before its leaf clocks can run. */
	if (cx_gdsc_power_on() != 0)
		return -2;

	/* 3. Root clock generators.  Linux asks for a 150 MHz hub clock on A690,
	      but the SC8280XP clock table starts at 200 MHz, so clk_set_rate()
	      rounds that request to this GPLL0/3 entry. */
	if (rcg_configure_enable(GPUCC_GMU_CLK_SRC_CMD,
			GPUCC_GMU_CLK_SRC_CFG, (6u << 8) | 2u) != 0) {
		klog("adreno_gpucc: gmu_clk_src failed to latch\n");
		return -3;
	}
	if (rcg_configure_enable(GPUCC_HUB_CLK_SRC_CMD, GPUCC_HUB_CLK_SRC_CFG,
			(5u << 8) | 5u) != 0) {
		klog("adreno_gpucc: hub_clk_src failed to latch\n");
		return -4;
	}

	/* 4. Every leaf listed by the GMU and GPU SMMU DT nodes is mandatory. */
	if (branch_enable(GPUCC_CXO_CLK) != 0 ||
	    branch_enable(GPUCC_AHB_CLK) != 0 ||
	    branch_enable(GPUCC_HUB_AON_CLK) != 0 ||
	    branch_enable(GPUCC_HUB_CX_INT_CLK) != 0 ||
	    branch_enable(GPUCC_CX_GMU_CLK) != 0 ||
	    branch_enable(GPUCC_HLOS1_VOTE_SMMU) != 0)
		return -5;
	/* GX_GMU is deliberately NOT enabled here: it lives in the GX domain,
	   which stays off until the GPU_SET OOB handshake asks the GMU firmware
	   to keep the GPU on.  adreno_gmu.c enables it after that handshake. */

	/* Keep the CB clock always on (gpucc probe()). */
	cc_set_bits(GPUCC_CB_CLK_ALWAYS_ON, BRANCH_CLK_EN, BRANCH_CLK_EN);

	adreno_mb();

	/* One line with the state that matters if a later GMU poll times out. */
	klog("adreno_gpucc: gmu_clk_src=%x/%x hub_clk_src=%x/%x cx_gdsc=%x hw=%x\n",
			cc_read(GPUCC_GMU_CLK_SRC_CMD), cc_read(GPUCC_GMU_CLK_SRC_CFG),
			cc_read(GPUCC_HUB_CLK_SRC_CMD), cc_read(GPUCC_HUB_CLK_SRC_CFG),
			cc_read(GPUCC_CX_GDSCR), cc_read(GPUCC_CX_GDSC_HW_CTRL));
	klog("adreno_gpucc: leaves cxo=%x ahb=%x hub_aon=%x hub_int=%x "
	     "cx_gmu=%x smmu=%x cb=%x div=%x/%x\n",
	     cc_read(GPUCC_CXO_CLK), cc_read(GPUCC_AHB_CLK),
	     cc_read(GPUCC_HUB_AON_CLK), cc_read(GPUCC_HUB_CX_INT_CLK),
	     cc_read(GPUCC_CX_GMU_CLK), cc_read(GPUCC_HLOS1_VOTE_SMMU),
	     cc_read(GPUCC_CB_CLK_ALWAYS_ON),
	     cc_read(0x11bcu), cc_read(0x11c0u));
	klog("adreno_gpucc: clocks enabled (gmu/hub 200 MHz, cx_gdsc %s)\n",
			(cc_read(GPUCC_CX_GDSC_HW_CTRL) & GDSC_PWR_ON) ? "on" : "OFF");
	return 0;
}

int adreno_gpucc_validate_runtime(void) {
	uint32_t axi = adreno_gcc_read(GCC_DDRSS_GPU_AXI_CLK);
	uint32_t memnoc = adreno_gcc_read(GCC_GPU_MEMNOC_GFX_CLK);
	uint32_t dvm = adreno_gcc_read(GCC_GPU_SNOC_DVM_GFX_CLK);

	if (!(axi & BRANCH_CLK_EN) || !branch_is_on(memnoc) ||
	    !branch_is_on(dvm)) {
		klog("adreno_gpucc: runtime clocks not ready axi=%x memnoc=%x dvm=%x\n",
		     axi, memnoc, dvm);
		return -1;
	}
	return 0;
}

/* Enable GPU_CC_GX_GMU_CLK once GPU_SET OOB has powered the GX rail.  Linux
   reaches this through the gx_gdsc power domain and the GPU clock list;
   enabling it earlier just leaves the branch halted (clk_branch2 polls
   CBCR_CLK_OFF and the GX domain is off). */
int adreno_gpucc_gx_clk_enable(void) {
	return branch_enable(GPUCC_GX_GMU_CLK);
}
