#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <ewoksys/dma.h>
#include <bsp/bsp_bpmp.h>

#include "ga10b.h"
#include "ga10b_regs.h"

/*
 * Power / detect / priv-ring / bus / early FB-LTC init.
 * Sequence transcribed from nvgpu rel-36 ga10b path
 * (common/init/nvgpu_init.c nvgpu_finalize_poweron):
 *   BPMP pg+clks+reset -> boot_0 -> fuses -> mask FUNC intrs ->
 *   priv ringmaster enum -> bus -> timer/CE CG -> FB/LTC CG -> fb init_hw.
 */

uint32_t ga10b_rd(uint32_t off) {
    if (!ga10b.hw || !ga10b.bar0) {
        klog("ga10b: [dry] rd  %08x\n", off);
        return 0;
    }
    return ga10b.bar0[off / 4];
}

void ga10b_wr(uint32_t off, uint32_t val) {
    if (!ga10b.hw || !ga10b.bar0) {
        klog("ga10b: [dry] wr  %08x = %08x\n", off, val);
        return;
    }
    ga10b.bar0[off / 4] = val;
}

void ga10b_wr_mask(uint32_t off, uint32_t mask, uint32_t val) {
    ga10b_wr(off, (ga10b_rd(off) & ~mask) | (val & mask));
}

int ga10b_poll(uint32_t off, uint32_t mask, uint32_t want, uint32_t tries) {
    if (!ga10b.hw)
        return 0;
    for (uint32_t i = 0; i < tries; i++) {
        if ((ga10b_rd(off) & mask) == want)
            return 0;
        usleep(100);
    }
    klog("ga10b: poll %08x mask %08x want %08x TIMEOUT (last=%08x)\n",
         off, mask, want, ga10b_rd(off));
    return -1;
}

void ga10b_delay_us(uint32_t us) {
    if (ga10b.hw)
        usleep(us);
}

int ga10b_map(void) {
    if (ga10b.mapped)
        return 0;
    if (!ga10b.hw) {
        klog("ga10b: [dry] map BAR0 %llx -> %llx (%llu MB)\n",
             GA10B_BAR0_PHY, GA10B_BAR0_VA, GA10B_BAR0_SIZE >> 20);
        return 0;
    }
    ewokos_addr_t ret = syscall3(SYS_MEM_MAP, (ewokos_addr_t)GA10B_BAR0_VA,
            (ewokos_addr_t)GA10B_BAR0_PHY, (ewokos_addr_t)GA10B_BAR0_SIZE);
    if (ret != (ewokos_addr_t)GA10B_BAR0_VA) {
        klog("ga10b: SYS_MEM_MAP BAR0 failed (%llx)\n", (uint64_t)ret);
        return -1;
    }
    ga10b.bar0 = (volatile uint32_t*)(uintptr_t)GA10B_BAR0_VA;
    ga10b.mapped = 1;
    return 0;
}

int ga10b_power_on(void) {
    if (ga10b.powered)
        return 0;
    if (!ga10b.hw) {
        klog("ga10b: [dry] bpmp init, pg_on(%d), clk_enable(%d,%d,%d), "
             "reset %d assert/deassert (GPC1 clk deferred to fuse check)\n",
             GA10B_PG_GPU, GA10B_CLK_GPUSYS, GA10B_CLK_GPC0,
             GA10B_CLK_FUSE, GA10B_RESET_GPU);
        ga10b.powered = 1;
        return 0;
    }
    if (bsp_bpmp_init() != 0) {
        klog("ga10b: bpmp init failed\n");
        return -1;
    }
    if (bsp_bpmp_pg_on(GA10B_PG_GPU) != 0) {
        klog("ga10b: pg_on(%d) failed\n", GA10B_PG_GPU);
        return -1;
    }
    uint32_t pgst = 0;
    int pgr = bsp_bpmp_pg_get_state(GA10B_PG_GPU, &pgst);
    klog("ga10b: pg(%d) state=%u (query r=%d)\n", GA10B_PG_GPU, pgst, pgr);
    static const uint32_t clks[] = {
        GA10B_CLK_GPUSYS, GA10B_CLK_GPC0, GA10B_CLK_FUSE
    };
    for (uint32_t i = 0; i < sizeof(clks) / sizeof(clks[0]); i++) {
        if (bsp_bpmp_clk_enable(clks[i]) != 0) {
            klog("ga10b: clk_enable(%u) failed\n", clks[i]);
            return -1;
        }
    }
    /* GPC1CLK is enabled later (ga10b_gpc_clk_init) only when the opt_gpc
     * fuse says GPC1 exists -- Orin Nano is a 1-GPC part and BPMP rejects
     * clk_enable(236) on floorswept silicon (nvgpu platform_ga10b_tegra.c
     * ga10b_tegra_is_clock_available). */
    /* Best-effort: BPMP may deny CCPLEX the GPU reset domain (EACCES) when
     * its power-on sequence already owns/deasserted it.  detect() reading a
     * valid boot_0 is the real arbiter of "GPU out of reset". */
    int ra = bsp_bpmp_reset_assert(GA10B_RESET_GPU);
    int rd = ra == 0 ? bsp_bpmp_reset_deassert(GA10B_RESET_GPU) : ra;
    if (ra != 0 || rd != 0)
        klog("ga10b: reset %d toggle denied (assert=%d deassert=%d), "
             "continuing -- boot_0 will tell\n", GA10B_RESET_GPU, ra, rd);
    usleep(1000);
    uint64_t rate = 0;
    bsp_bpmp_clk_get_rate(GA10B_CLK_GPUSYS, &rate);
    ga10b.gpusys_hz = rate;
    ga10b.powered = 1;
    klog("ga10b: powered, gpusys=%llu Hz\n", rate);
    return 0;
}

int ga10b_power_off(void) {
    if (!ga10b.powered)
        return 0;
    if (!ga10b.hw) {
        klog("ga10b: [dry] reset %d assert, pg_off(%d)\n",
             GA10B_RESET_GPU, GA10B_PG_GPU);
        ga10b.powered = 0;
        return 0;
    }
    bsp_bpmp_reset_assert(GA10B_RESET_GPU);
    bsp_bpmp_pg_off(GA10B_PG_GPU);
    ga10b.powered = 0;
    return 0;
}

int ga10b_detect(void) {
    if (!ga10b.hw) {
        klog("ga10b: [dry] detect: assume GA10B (boot_0 arch=0x17 impl=0xB)\n");
        ga10b.chip_id = GA10B_GPUID_GA10B;
        return 0;
    }
    uint32_t v = ga10b_rd(GA10B_MC_BOOT_0);
    if (v == 0xFFFFFFFFu) {
        klog("ga10b: boot_0=ffffffff -- GPU not powered/clocked\n");
        return -1;
    }
    ga10b.chip_id = (v >> GA10B_BOOT_0_IMPL_SHIFT) & GA10B_BOOT_0_CHIP_MASK;
    klog("ga10b: boot_0=%08x chip_id=%03x\n", v, ga10b.chip_id);
    if (ga10b.chip_id != GA10B_GPUID_GA10B) {
        klog("ga10b: not GA10B (want %03x)\n", GA10B_GPUID_GA10B);
        return -1;
    }
    return 0;
}

int ga10b_fuse_read(void) {
    if (!ga10b.hw) {
        klog("ga10b: [dry] fuses: assume retail secured silicon "
             "(priv_sec_en=1, wpr_en=1)\n");
        ga10b.priv_sec_en = 1;
        return 0;
    }
    ga10b.priv_sec_en = ga10b_rd(GA10B_FUSE_PRIV_SEC_EN) != 0;
    uint32_t dbg = ga10b_rd(GA10B_FUSE_SEC_DEBUG_EN);
    klog("ga10b: priv_sec_en=%d sec_debug=%08x\n", ga10b.priv_sec_en, dbg);
    if (!ga10b.priv_sec_en)
        return 0;
    /* ga10b_fuse_check_priv_security: secured silicon prerequisites */
    if (ga10b_rd(GA10B_FUSE_SRC_ISOLATION_EN) == 0) {
        klog("ga10b: fuse src_isolation_en=0 but priv_sec_en=1\n");
        return -1;
    }
    if (!(ga10b_rd(GA10B_FUSE_WPR_ENABLED) & 1)) {
        klog("ga10b: fuse wpr_enabled=0 but priv_sec_en=1\n");
        return -1;
    }
    if (ga10b_rd(GA10B_FUSE_VPR_AUTO_FETCH_DIS) & 1) {
        klog("ga10b: fuse vpr_auto_fetch_dis=1 but priv_sec_en=1\n");
        return -1;
    }
    klog("ga10b: vpr_enabled=%08x -- secured boot path (ACR required)\n",
         ga10b_rd(GA10B_FUSE_VPR_ENABLED));
    return 0;
}

/* GPC1 clock is fuse-gated: read NV_PGPU_FUSE_STATUS_OPT_GPC (bit=1 means
 * floorswept) and only enable GPC1CLK when GPC1 exists.  Needs BAR0 mapped
 * + fuse clock on + GPU out of reset, so it runs after ga10b_fuse_read. */
int ga10b_gpc_clk_init(void) {
    if (!ga10b.hw) {
        klog("ga10b: [dry] gpc clk: assume 2 GPCs, clk_enable(%d)\n",
             GA10B_CLK_GPC1);
        ga10b.gpc_count = 2;
        return 0;
    }
    uint32_t dis = ga10b_rd(GA10B_FUSE_STATUS_OPT_GPC);
    ga10b.gpc_disable = dis;
    ga10b.gpc_count = 0;
    for (uint32_t i = 0; i < GA10B_MAX_GPC; i++)
        if (!(dis & (1u << i)))
            ga10b.gpc_count++;
    klog("ga10b: opt_gpc_disable=%08x -> %u GPC(s)\n", dis, ga10b.gpc_count);
    if (!(dis & 2u)) {
        if (bsp_bpmp_clk_enable(GA10B_CLK_GPC1) != 0) {
            klog("ga10b: clk_enable(%u) failed\n", GA10B_CLK_GPC1);
            return -1;
        }
    }
    return 0;
}

int ga10b_priv_ring_init(void) {
    static const uint32_t slcg[] = {
        GA10B_PRIM_SLCG, GA10B_RS_CTRL_FBP_SLCG, GA10B_RS_CTRL_GPC_SLCG,
        GA10B_RS_CTRL_SYS_SLCG, GA10B_RS_FBP0_SLCG, GA10B_RS_FBP1_SLCG,
        GA10B_RS_GPC0_SLCG, GA10B_RS_GPC1_SLCG, GA10B_RS_SYS_SLCG
    };
    for (uint32_t i = 0; i < sizeof(slcg) / sizeof(slcg[0]); i++)
        ga10b_wr(slcg[i], 0);

    ga10b_wr(GA10B_PRIM_CMD, GA10B_PRIM_CMD_ENUM_START);
    if (ga10b.hw) {
        for (uint32_t i = 0; i < 60; i++) {
            if (ga10b_rd(GA10B_PRIM_CMD) == 0)
                break;
            usleep(500);
        }
        if (ga10b_rd(GA10B_PRIM_CMD) != 0) {
            klog("ga10b: priv ring enum cmd timeout\n");
            return -1;
        }
        if (!(ga10b_rd(GA10B_PRIM_START_RESULTS) & 1)) {
            klog("ga10b: priv ring enum failed (results=%08x intr=%08x)\n",
                 ga10b_rd(GA10B_PRIM_START_RESULTS),
                 ga10b_rd(GA10B_PRIM_INTR_STATUS0));
            return -1;
        }
    }
    klog("ga10b: priv ring up: ltc=%u fbp=%u gpc=%u\n",
         ga10b_rd(GA10B_PRIM_ENUM_LTC) & 0x1f,
         ga10b_rd(GA10B_PRIM_ENUM_FBP) & 0x1f,
         ga10b_rd(GA10B_PRIM_ENUM_GPC) & 0x1f);
    return 0;
}

int ga10b_bus_init(void) {
    ga10b_wr(GA10B_BUS_INTR_0, GA10B_BUS_INTR_EN_VAL);   /* W1C pending */
    ga10b_wr(GA10B_BUS_INTR_EN_0, GA10B_BUS_INTR_EN_VAL);
    return 0;
}

int ga10b_cg_early_init(void) {
    /* ga10b: MC_ENABLE FIFO reset is a no-op; only CG prods remain here.
     * Runlist CG prods are per-runlist (ga10b_fifo_init); mc_elpg_enable is
     * BPMP-owned on silicon and must not be written. */
    ga10b_wr(GA10B_TIMER_SLCG, 0);
    ga10b_wr(GA10B_CE2_SLCG, 0);
    return 0;
}

int ga10b_mc_intr_mask(void) {
    /* FUNC tree: mask every top vector; the driver runs polled, so
     * nothing is unmasked afterwards. */
    ga10b_wr(GA10B_FUNC_BASE + GA10B_FUNC_INTR_TOP_EN_CLR(0), 0xFFFFFFFFu);
    return 0;
}

/* cfg num_rows counts ROWS, not devices (ga10b_top_table_size); the row
 * walk itself is ga10b_devtable_parse (ga10b.h). */
#define TOP_ROWS_MAX 192u
int ga10b_device_table(ga10b_devtable_t* t) {
    static uint32_t rows[TOP_ROWS_MAX];
    memset(t, 0, sizeof(*t));
    uint32_t cfg = ga10b_rd(GA10B_TOP_DEVICE_INFO_CFG);
    if ((cfg & 0xf) != 2) {
        klog("ga10b: device_info_cfg ver=%u (want 2)\n", cfg & 0xf);
        return ga10b.hw ? -1 : 0;
    }
    uint32_t nrows = (cfg >> 20) & 0xFFFu;
    if (nrows > TOP_ROWS_MAX) {
        klog("ga10b: device table %u rows, parsing first %u\n", nrows,
             TOP_ROWS_MAX);
        nrows = TOP_ROWS_MAX;
    }
    for (uint32_t i = 0; i < nrows; i++)
        rows[i] = ga10b_rd(GA10B_TOP_DEVICE_INFO2(i));
    ga10b_devtable_parse(rows, nrows, t);
    for (uint32_t i = 0; i < t->count; i++) {
        const ga10b_devinfo_t* d = &t->dev[i];
        klog("ga10b: dev[%u] type=%u inst=%u pri=%08x reset=%u eng=%d "
             "rl=%08x rleng=%u\n", i, d->type, d->inst_id, d->pri_base,
             d->reset_id, d->is_engine, d->runlist_base, d->rleng_id);
    }
    return 0;
}

/* ga10b_mc_enable_dev: mc_device_enable has a single register, so only
 * reset ids 0..31 exist.  Like nvgpu, a readback mismatch is reported but
 * not fatal (BPMP's DEVICE_ELPG_ENABLE also gates the engine state). */
int ga10b_engine_enable(uint32_t reset_id, int enable) {
    if (reset_id >= 32u) {
        klog("ga10b: reset_id %u out of range\n", reset_id);
        return -1;
    }
    uint32_t bit = 1u << reset_id;
    ga10b_wr_mask(GA10B_MC_DEVICE_ENABLE, bit, enable ? bit : 0);
    if (ga10b_poll(GA10B_MC_DEVICE_ENABLE, bit, enable ? bit : 0, 5) != 0)
        klog("ga10b: device_enable bit %u did not %s\n", reset_id,
             enable ? "set" : "clear");
    return 0;
}

int ga10b_engine_reset(uint32_t reset_id) {
    if (ga10b_engine_enable(reset_id, 0) != 0)
        return -1;
    return ga10b_engine_enable(reset_id, 1);
}

int ga10b_fb_ltc_early_init(void) {
    ga10b_wr(GA10B_FB_SLCG_1, 0x10c20);
    ga10b_wr(GA10B_FB_SLCG_2, 0);
    ga10b_wr(GA10B_FB_BLCG_1, 0xc270);
    ga10b_wr(GA10B_FB_BLCG_2, 0xc270);
    ga10b_wr(GA10B_FB_BLCG_3, 0x4242);
    if (!ga10b.priv_sec_en)
        ga10b_wr(GA10B_FB_PRIV_MMU_PHY_SECURE, 0xFFFFFFFFu);
    uint32_t ltc = ga10b_rd(GA10B_PRIM_ENUM_LTC) & 0x1f;
    klog("ga10b: ltc enum=%u fb_mmu=%08x fbhub=%08x\n", ltc,
         ga10b_rd(GA10B_FB_MMU_NUM_ACTIVE_LTCS),
         ga10b_rd(GA10B_FB_FBHUB_NUM_ACTIVE_LTCS));
    return 0;
}

/* gm20b_fb_init_hw: sysmem flush page + SEPARATE mmu debug wr/rd pages
 * (mm.mmu_wr_mem / mm.mmu_rd_mem), sys_ncoh aperture, vol=false. */
int ga10b_fb_init_hw(void) {
    static void* va[3];
    static uint64_t phy[3];
    for (uint32_t i = 0; i < 3; i++) {
        if (va[i] != NULL)
            continue;
        if (ga10b_dma_alloc(4096, 4096, &va[i], &phy[i]) != 0)
            return -1;
        memset(va[i], 0, 4096);
    }
    ga10b_wr(GA10B_FB_NISO_FLUSH_SYSMEM_ADDR, (uint32_t)(phy[0] >> 8));
    /* mmu debug: aperture sys_ncoh(3) | ((pa>>12)&0xFFFFFFF)<<4 */
    ga10b_wr(GA10B_FB_MMU_DEBUG_WR,
             3u | (((uint32_t)(phy[1] >> 12) & 0xFFFFFFFu) << 4));
    ga10b_wr(GA10B_FB_MMU_DEBUG_RD,
             3u | (((uint32_t)(phy[2] >> 12) & 0xFFFFFFFu) << 4));
    return 0;
}
