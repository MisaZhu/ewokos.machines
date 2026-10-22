/*
 * adreno_rpmh.c - minimal RPMh vote for the Adreno GFX power domain.
 *
 * The GPUCC is a child of the RPMh power domain "gfx.lvl" (sc8280xp.dtsi:
 * gpucc { power-domains = <&rpmhpd SC8280XP_GFX> }).  Before the first
 * GPUCC register is touched, gpucc-sc8280xp.c probe() does
 *
 *     pm_runtime_get_sync(&pdev->dev);      // vote gfx.lvl
 *     qcom_branch_set_clk_en(regmap, 0x109c);
 *     qcom_cc_really_probe(...);
 *
 * EwokOS has no genpd/RPMh stack, so without this vote the GFX power
 * domain (and the GPU NoC ports behind it) stays in whatever state the
 * bootloader left.  On the Q8B that shows up as gcc_gpu_memnoc_gfx_clk /
 * snoc_dvm / tcu reporting CBCR_CLK_OFF even after the branches are
 * enabled, and the GMU CM3 faults the moment its reset is released
 * (its first instruction fetch needs the icache in DRAM through the GMU
 * SMMU + NoC).
 *
 * This file is a small, self-contained copy of the user-space command-DB
 * reader and RPMh TCS writer that the Q8B SDCC2 driver already uses
 * (system/libs/bsp/src/sdcc2.c), plus the rpmpd corner selection:
 *
 *   - cmd-db lives at 0x80860000 (DT cmd-db-region, whitelisted for user
 *     space by check_mem_map_arch -> "user-space SDCC2 resolves the rails").
 *   - RPMh commands go through the apps RSC drv-2 TCS at 0x18220000
 *     (low MMIO, already used by the kernel's SD rail code).
 *   - rpmpd writes the *corner index* (not the ARC level value) as the
 *     32-bit payload; the index is derived from the cmd-db aux data
 *     ("gfx.lvl" levels), skipping RPMH_REGULATOR_LEVEL_RETENTION (16)
 *     exactly like rpmhpd_parse_arc_level().
 *
 * The vote is best-effort: if the command DB or RSC window cannot be
 * mapped the caller logs and continues (the GPU path then likely still
 * declines, but nothing else is disturbed).
 */
#include <adreno/adreno_drv.h>
#include <adreno/adreno_reg.h>
#include <q8b_adreno.h>
#include <ewoksys/mmio.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <ewoksys/syscall.h>
#include <syscalls.h>
#include <stdlib.h>

/* --- command DB (DT cmd-db-region@80860000) --------------------------- */
#define CMDDB_VA            (ADRENO_MMIO_VA_BASE + 0x00300000ULL)
#define CMDDB_PHY           0x80860000ULL
#define CMDDB_SIZE          0x00020000u

#define CMD_DB_DATA_OFFSET  144u
#define CMD_DB_RSC_OFFSET   8u
#define CMD_DB_RSC_SIZE     16u
#define CMD_DB_ENTRY_SIZE   24u
#define CMD_DB_MAX_SLAVES   8u
#define CMD_DB_MAGIC        0x0c0330dbu

/* --- apps RSC drv-2 TCS (same block the SD rail code drives) ---------- */
#define RSC_VA              (ADRENO_MMIO_VA_BASE + 0x00400000ULL)
#define RSC_PHY             0x18220000ULL
#define RSC_SIZE            0x00010000u

#define RPMH_TCS_OFF        0x0d00u
#define RPMH_TCS_STRIDE     672u
#define RPMH_IRQ_ENABLE     0x00u
#define RPMH_IRQ_STATUS     0x04u
#define RPMH_IRQ_CLEAR      0x08u
#define RPMH_AMC_ENABLE     (1u << 16)
#define RPMH_AMC_TRIGGER    (1u << 24)
#define RPMH_WRITE_RESP_MSGID 0x00010108u

/* TCS_CMD_STATUS bits (drivers/soc/qcom/rpmh-rsc.c). */
#define RPMH_CMD_STATUS_ISSUED (1u << 8)
#define RPMH_CMD_STATUS_COMPL  (1u << 16)

/* The TCS register offsets depend on the RSC hardware version, read from
   RSC_DRV_ID (offset 0): v2.7 uses control 0x14/cmd stride 20, v3.0 uses
   0x24/24 (drivers/soc/qcom/rpmh-rsc.c reg_offset_ver_{2_7,3_0}). */
static struct {
	uint32_t control, status, cmd_enable;
	uint32_t cmd_msgid, cmd_addr, cmd_data, cmd_status;
	uint32_t cmd_stride;
	int major, minor;
} rsc = {
	.control = 0x14u, .status = 0x18u, .cmd_enable = 0x1cu,
	.cmd_msgid = 0x30u, .cmd_addr = 0x34u, .cmd_data = 0x38u,
	.cmd_status = 0x3cu, .cmd_stride = 20u,
};

/* rpmhpd_parse_arc_level() skips the retention corner. */
#define RPMH_REGULATOR_LEVEL_RETENTION 16u

static int _mapped;

static int rpmh_map(void)
{
	ewokos_addr_t ret;

	if (_mapped)
		return 0;
	ret = syscall3(SYS_MEM_MAP, CMDDB_VA, CMDDB_PHY, CMDDB_SIZE);
	if (ret != CMDDB_VA)
		return -1;
	ret = syscall3(SYS_MEM_MAP, RSC_VA, RSC_PHY, RSC_SIZE);
	if (ret != RSC_VA)
		return -2;
	_mapped = 1;
	return 0;
}

static uint8_t cd8(uint32_t off)
{
	return *(volatile uint8_t *)(CMDDB_VA + off);
}

static uint16_t cd16(uint32_t off)
{
	volatile uint8_t *p = (volatile uint8_t *)(CMDDB_VA + off);
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t cd32(uint32_t off)
{
	return get32(CMDDB_VA + off);
}

static int cmd_db_id_matches(uint32_t entry, const char *id)
{
	uint32_t i;
	for (i = 0; i < 8u; i++) {
		uint8_t expected = (uint8_t)id[i];
		if (cd8(entry + i) != expected)
			return 0;
		if (expected == 0)
			return 1;
	}
	return 1;   /* full 8-char id matched */
}

/*
 * Locate a cmd-db resource.  Returns 0 and fills addr, the aux-data
 * offset (relative to CMDDB_VA) and its byte length.
 */
static int cmd_db_find(const char *id, uint32_t *addr,
		uint32_t *aux_off, uint32_t *aux_len)
{
	uint32_t i;

	if (cd32(4u) != CMD_DB_MAGIC)
		return -1;

	for (i = 0; i < CMD_DB_MAX_SLAVES; i++) {
		uint32_t rsc = CMD_DB_RSC_OFFSET + i * CMD_DB_RSC_SIZE;
		uint32_t slave_id = cd16(rsc);
		uint32_t header_off, data_off, count, entries, j;

		if (slave_id == 0)
			break;
		header_off = cd16(rsc + 2u);
		data_off   = cd16(rsc + 4u);
		count      = cd16(rsc + 6u);
		entries    = CMD_DB_DATA_OFFSET + header_off;
		if (entries >= CMDDB_SIZE ||
		    count > (CMDDB_SIZE - entries) / CMD_DB_ENTRY_SIZE)
			return -1;

		for (j = 0; j < count; j++) {
			uint32_t entry = entries + j * CMD_DB_ENTRY_SIZE;
			if (!cmd_db_id_matches(entry, id))
				continue;
			if (addr)
				*addr = cd32(entry + 16u);
			if (aux_off)
				*aux_off = CMD_DB_DATA_OFFSET + data_off +
					cd16(entry + 22u);
			if (aux_len)
				*aux_len = cd16(entry + 20u);
			return 0;
		}
	}
	return -1;
}

/* Build one GMU ARC vote exactly as a6xx_gmu_rpmh_arc_votes_init() does.
 * The primary table belongs to the voted domain (gfx.lvl or cx.lvl); the
 * secondary index always comes from mx.lvl. */
int adreno_rpmh_arc_vote(const char *id, uint32_t level, uint32_t *vote)
{
	uint32_t pri_off = 0, pri_len = 0, sec_off = 0, sec_len = 0;
	uint32_t pri_count, sec_count, i;
	uint32_t pindex = 0, sindex = 0, primary_level;

	if (!id || !vote)
		return -1;
	if (rpmh_map() != 0)
		return -2;
	if (cmd_db_find(id, 0, &pri_off, &pri_len) != 0 ||
	    cmd_db_find("mx.lvl", 0, &sec_off, &sec_len) != 0)
		return -3;

	pri_count = pri_len / 2u;
	sec_count = sec_len / 2u;
	if (!pri_count || !sec_count || pri_count > 256u || sec_count > 256u)
		return -4;

	for (i = 0; i < pri_count; i++) {
		if (cd16(pri_off + i * 2u) >= level) {
			pindex = i;
			break;
		}
	}
	if (i == pri_count) {
		klog("adreno_rpmh: %s has no ARC level >= %u\n", id, level);
		return -5;
	}

	/* Use the first MX level at or above the OPP level.  If MX tops out
	 * below it, Linux retains the highest non-zero MX index. */
	for (i = 0; i < sec_count; i++) {
		uint32_t secondary_level = cd16(sec_off + i * 2u);
		if (secondary_level >= level) {
			sindex = i;
			break;
		} else if (secondary_level) {
			sindex = i;
		}
	}

	primary_level = cd16(pri_off + pindex * 2u);
	*vote = ((primary_level & 0xffffu) << 16) |
		((sindex & 0xffu) << 8) | (pindex & 0xffu);
	return 0;
}

/* --- RPMh TCS write (copy of the SDCC2 user-space sequence) ----------- */
static void rsc_detect(void)
{
	uint32_t id = get32(RSC_VA);   /* RSC_DRV_ID at offset 0 */
	int major = (int)((id >> 16) & 0xffu);

	rsc.major = major;
	rsc.minor = (int)((id >> 8) & 0xffu);
	if (major >= 3) {
		rsc.control    = 0x24u;
		rsc.status     = 0x28u;
		rsc.cmd_enable = 0x2cu;
		rsc.cmd_msgid  = 0x34u;
		rsc.cmd_addr   = 0x38u;
		rsc.cmd_data   = 0x3cu;
		rsc.cmd_status = 0x40u;
		rsc.cmd_stride = 24u;
	}
	klog("adreno_rpmh: rsc id=%x v%d.%d layout=%s\n",
	     id, rsc.major, rsc.minor, major >= 3 ? "3.0" : "2.7");
}

static uint32_t rpmh_reg(uint32_t tcs, uint32_t reg)
{
	return get32(RSC_VA + RPMH_TCS_OFF + tcs * RPMH_TCS_STRIDE + reg);
}

static void rpmh_wr(uint32_t tcs, uint32_t reg, uint32_t val)
{
	put32(RSC_VA + RPMH_TCS_OFF + tcs * RPMH_TCS_STRIDE + reg, val);
}

static uint32_t rpmh_cmd(uint32_t tcs, uint32_t cmd, uint32_t reg)
{
	return RPMH_TCS_OFF + tcs * RPMH_TCS_STRIDE +
		cmd * rsc.cmd_stride + reg;
}

static int rpmh_wr_sync(uint32_t tcs, uint32_t reg, uint32_t val)
{
	int i;
	rpmh_wr(tcs, reg, val);
	for (i = 0; i < 100000; i++) {
		if (rpmh_reg(tcs, reg) == val)
			return 0;
		proc_nsleep_precise(1000);
	}
	return -1;
}

static void rpmh_finish(uint32_t tcs)
{
	uint32_t control = rpmh_reg(tcs, rsc.control);
	(void)rpmh_wr_sync(tcs, rsc.control, control & ~RPMH_AMC_TRIGGER);
	(void)rpmh_wr_sync(tcs, rsc.control, 0);
	(void)rpmh_wr_sync(tcs, rsc.cmd_enable, 0);
	put32(RSC_VA + RPMH_IRQ_CLEAR, 1u << tcs);
}

static int rpmh_write(uint32_t address, uint32_t data)
{
	const uint32_t tcs = 0;
	const uint32_t irq_bit = 1u << tcs;
	uint32_t control;
	int i;

	control = rpmh_reg(tcs, rsc.control);
	control &= ~RPMH_AMC_TRIGGER;
	if (rpmh_wr_sync(tcs, rsc.control, control) != 0)
		goto fail;
	control &= ~RPMH_AMC_ENABLE;
	if (rpmh_wr_sync(tcs, rsc.control, control) != 0)
		goto fail;
	if (rpmh_wr_sync(tcs, rsc.cmd_enable, 0) != 0)
		goto fail;

	put32(RSC_VA + RPMH_IRQ_CLEAR, irq_bit);
	put32(RSC_VA + RPMH_IRQ_ENABLE,
	      get32(RSC_VA + RPMH_IRQ_ENABLE) | irq_bit);

	put32(RSC_VA + rpmh_cmd(tcs, 0, rsc.cmd_msgid), RPMH_WRITE_RESP_MSGID);
	put32(RSC_VA + rpmh_cmd(tcs, 0, rsc.cmd_addr), address);
	put32(RSC_VA + rpmh_cmd(tcs, 0, rsc.cmd_data), data);

	if (rpmh_wr_sync(tcs, rsc.cmd_enable, 1u) != 0)
		goto fail;
	if (rpmh_wr_sync(tcs, rsc.control, RPMH_AMC_ENABLE) != 0)
		goto fail;
	put32(RSC_VA + RPMH_TCS_OFF + tcs * RPMH_TCS_STRIDE + rsc.control,
	      RPMH_AMC_ENABLE | RPMH_AMC_TRIGGER);

	for (i = 0; i < 1000000; i++) {
		uint32_t irq = get32(RSC_VA + RPMH_IRQ_STATUS);
		uint32_t cs = get32(RSC_VA + rpmh_cmd(tcs, 0, rsc.cmd_status));
		if (irq & irq_bit) {
			rpmh_finish(tcs);
			return 0;
		}
		/* Some RSC revisions do not latch the TCS IRQ status for a
		   fire-and-forget AMC write; CMD_STATUS.COMPL is authoritative. */
		if (cs & RPMH_CMD_STATUS_COMPL) {
			klog("adreno_rpmh: write completed (irq status=%x)\n", irq);
			rpmh_finish(tcs);
			return 0;
		}
		proc_nsleep_precise(1000);
	}

fail:
	klog("adreno_rpmh: write addr=%x data=%x failed (ctrl=%x st=%x cmd=%x irq=%x en=%x)\n",
	     address, data, rpmh_reg(tcs, rsc.control),
	     rpmh_reg(tcs, rsc.status),
	     get32(RSC_VA + rpmh_cmd(tcs, 0, rsc.cmd_status)),
	     get32(RSC_VA + RPMH_IRQ_STATUS),
	     get32(RSC_VA + RPMH_IRQ_ENABLE));
	rpmh_finish(tcs);
	return -1;
}

/*
 * Vote one RPMh power domain at its highest valid corner (rpmpd clamps every
 * vote to level_count-1 until sync_state, which is exactly what Linux's
 * pm_runtime_get_sync() does during GPUCC/GMU probe).
 */
static int rpmh_vote_pd(const char *name)
{
	uint32_t addr = 0, aux_off = 0, aux_len = 0;
	uint32_t i, n, count;
	int corner;

	if (cmd_db_find(name, &addr, &aux_off, &aux_len) != 0 || addr == 0) {
		klog("adreno_rpmh: %s not found in cmd-db\n", name);
		return -2;
	}

	n = aux_len / 2u;
	/* rpmhpd_parse_arc_level(): level_count stops at the first zero entry
	   after index 0 (the aux data is zero padded). */
	count = n;
	for (i = 1; i < n; i++) {
		if (cd16(aux_off + i * 2u) == 0) {
			count = i;
			break;
		}
	}
	if (count == 0)
		count = 1;

	corner = (int)count - 1;

	klog("adreno_rpmh: %s addr=%x levels=%u corner=%d\n",
	     name, addr, count, corner);
	if (n > 0) {
		klog("adreno_rpmh: %s levels %u %u %u %u %u %u %u %u\n", name,
		     n > 0 ? cd16(aux_off + 0) : 0,
		     n > 1 ? cd16(aux_off + 2) : 0,
		     n > 2 ? cd16(aux_off + 4) : 0,
		     n > 3 ? cd16(aux_off + 6) : 0,
		     n > 4 ? cd16(aux_off + 8) : 0,
		     n > 5 ? cd16(aux_off + 10) : 0,
		     n > 6 ? cd16(aux_off + 12) : 0,
		     n > 7 ? cd16(aux_off + 14) : 0);
	}
	if (n > 8) {
		klog("adreno_rpmh: %s levels %u %u %u %u %u %u %u %u\n", name,
		     n > 8 ? cd16(aux_off + 16) : 0,
		     n > 9 ? cd16(aux_off + 18) : 0,
		     n > 10 ? cd16(aux_off + 20) : 0,
		     n > 11 ? cd16(aux_off + 22) : 0,
		     n > 12 ? cd16(aux_off + 24) : 0,
		     n > 13 ? cd16(aux_off + 26) : 0,
		     n > 14 ? cd16(aux_off + 28) : 0,
		     n > 15 ? cd16(aux_off + 30) : 0);
	}

	if (rpmh_write(addr, (uint32_t)corner) != 0)
		return -3;
	klog("adreno_rpmh: %s voted corner %d\n", name, corner);
	return 0;
}

/*
 * Power the two RPMh domains the GMU device hangs off (sc8280xp.dtsi gmu
 * node: power-domains = <&gpucc GPU_CC_CX_GDSC>, <&gpucc GPU_CC_GX_GDSC>;
 * the GPUCC itself is a child of rpmpd SC8280XP_GFX).  Linux reaches this
 * through pm_runtime_get_sync() on the GPUCC and GMU devices before the CM3
 * is released; without it the GMU's first GX power-up can brown out the
 * board.
 */
/* bcm-voter.c / tcs.h fields. */
#define BCM_TCS_COMMIT        (1u << 30)
#define BCM_TCS_VALID         (1u << 29)
#define BCM_TCS_VOTE_MASK     0x3fffu

/* Submit the peak-bandwidth vote generated by bcm_aggregate().  Command-DB
 * bcm_db aux data is { le32 unit, le16 width, u8 vcd, u8 reserved } and the
 * qcom provider's default vote_scale is 1000. */
static int rpmh_bcm_peak(const char *name, uint32_t peak_kbps,
		uint32_t buswidth)
{
	uint32_t addr = 0, aux_off = 0, aux_len = 0;
	uint32_t unit, width, vote, data;
	uint64_t scaled;

	if (cmd_db_find(name, &addr, &aux_off, &aux_len) != 0 || addr == 0 ||
	    aux_len < 8u || buswidth == 0) {
		klog("adreno_rpmh: bcm %s missing/invalid in cmd-db\n", name);
		return -2;
	}
	unit = cd32(aux_off);
	width = cd16(aux_off + 4u);
	if (!unit || !width)
		return -3;

	scaled = (uint64_t)peak_kbps * width;
	scaled /= buswidth;
	scaled *= 1000u;
	scaled /= unit;
	if (!scaled)
		scaled = 1;
	vote = scaled > BCM_TCS_VOTE_MASK ? BCM_TCS_VOTE_MASK : (uint32_t)scaled;
	data = BCM_TCS_COMMIT | BCM_TCS_VALID | vote;

	klog("adreno_rpmh: bcm %s addr=%x unit=%u width=%u vcd=%u peak=%u vote=%x\n",
	     name, addr, unit, width, cd8(aux_off + 6u), peak_kbps, data);
	return rpmh_write(addr, data);
}

/* bcm_acv has enable_mask=BIT(3): any EBI bandwidth turns it on. */
static int rpmh_bcm_acv_enable(void)
{
	uint32_t addr = 0, aux_off = 0, aux_len = 0;
	uint32_t data = BCM_TCS_COMMIT | BCM_TCS_VALID | (1u << 3);

	if (cmd_db_find("ACV", &addr, &aux_off, &aux_len) != 0 || addr == 0) {
		klog("adreno_rpmh: bcm ACV not found in cmd-db\n");
		return -2;
	}
	klog("adreno_rpmh: bcm ACV addr=%x vcd=%u vote=%x\n",
	     addr, aux_len >= 7u ? cd8(aux_off + 6u) : 0, data);
	return rpmh_write(addr, data);
}

/* a6xx_gmu_set_initial_bw(): the GPU's gfx-mem interconnect vote, issued
   before the CM3 release.  Without it the gem_noc port for the GPU stays
   unpowered (the gcc_gpu_memnoc_gfx_clk branch is vote-controlled), and the
   GMU's first DRAM fetch can lock the NoC. */
int adreno_rpmh_interconnect_vote(void);

static int rpmh_gfx_interconnect(void)
{
	/* sc8280xp GPU OPP 690 MHz: opp-peak-kBps = 2736000.  The path is
	 * qnm_gpu -> qns_llcc (SH0, buswidth 16) -> llcc_mc -> ebi
	 * (ACV + MC0, buswidth 4). */
	const uint32_t peak_kbps = 2736000u;
	int rc = 0;
	if (rpmh_bcm_peak("SH0", peak_kbps, 16) != 0) rc = -1;
	if (rpmh_bcm_acv_enable() != 0) rc = -1;
	if (rpmh_bcm_peak("MC0", peak_kbps, 4) != 0) rc = -1;
	return rc;
}

/* a6xx_gmu_set_initial_bw(): the gfx-mem interconnect vote.  Linux issues
   this in a6xx_gmu_resume AFTER clk_bulk_prepare_enable(), because the GCC
   GPU NoC branches are vote-controlled and the NoC only raises its port
   clock when the branch is already enabled as the vote arrives. */
int adreno_rpmh_interconnect_vote(void)
{
	return rpmh_gfx_interconnect();
}

/* SC8280XP SCM's DT path: MASTER_CRYPTO -> A2NOC -> SNOC_SF ->
 * LLCC -> EBI. Linux qcom_scm_bw_enable() requests peak UINT_MAX.
 * sc8280xp.c associates these nodes with CE0, SN4, SN0, SH0, MC0/ACV.
 * Keep these votes through authentication; this port has no suspend yet. */
int adreno_rpmh_scm_vote(void)
{
	if (rpmh_map() != 0)
		return -1;
	rsc_detect();
	if (rpmh_bcm_peak("CE0", UINT32_MAX, 8) != 0 ||
	    rpmh_bcm_peak("SN4", UINT32_MAX, 16) != 0 ||
	    rpmh_bcm_peak("SN0", UINT32_MAX, 16) != 0 ||
	    rpmh_bcm_peak("SH0", UINT32_MAX, 16) != 0 ||
	    rpmh_bcm_acv_enable() != 0 ||
	    rpmh_bcm_peak("MC0", UINT32_MAX, 4) != 0)
		return -1;
	return 0;
}

int adreno_rpmh_gfx_vote(void)
{
	int rc = 0;

	if (rpmh_map() != 0) {
		klog("adreno_rpmh: cmd-db/RSC window map failed\n");
		return -1;
	}
	rsc_detect();

	if (rpmh_vote_pd("cx.lvl") != 0)
		rc = -1;
	if (rpmh_vote_pd("gfx.lvl") != 0)
		rc = -1;
	return rc;
}
