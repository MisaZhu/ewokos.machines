/*
 * test_integration.c -- host integration dry-run of the REAL driver flow.
 *
 * Links the actual ga10b_gr.c / ga10b_fifo.c / ga10b_mm.c / ga10b_ce.c /
 * ga10b_fw_parse.c against host_shim.c (dry-run OS semantics) and runs
 *   fifo_init -> gr_enable_hw (pre-ACR) -> gr_init (full golden-ctx
 *   sequence) -> channels -> compute
 *   dispatch x2 -> hw-mode ctx_prepare guard -> CE blit
 * asserting the captured MMIO trace against the real NETC blob contents
 * and the DMA-memory structures (RAMFC/RAMIN/headers/GPFIFO/USERD).
 *
 * Build: cc -I. -Itest -Itest/host -Wall -Wextra -o /tmp/test_integration \
 *   test/test_integration.c test/host_shim.c ga10b_gr.c ga10b_fifo.c \
 *   ga10b_mm.c ga10b_ce.c ga10b_fw_parse.c
 * Run:   /tmp/test_integration [fw_dir]   (default /Users/tony/work/gpu-fw/ga10b)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ga10b.h"
#include "ga10b_regs.h"
#include "ga10b_fifo.h"
#include "ga10b_ctx.h"
#include "ga10b_netlist.h"
#include "host_shim.h"
#include "ga10b_gating.h"

static int fails;
#define EXPECT(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } \
} while (0)

static void* load_file(const char* dir, const char* name, uint32_t* size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* buf = malloc((size_t)len);
    if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (uint32_t)len;
    return buf;
}

/* full gfx-only filter table (ga10b_gr_init_is_allowed_reg, MIG path) --
 * the test pins that the driver applies exactly this set */
static int in_gfx(uint32_t a) {
    static const uint32_t gfx[][2] = {
        { 0x00405800, 0x00405864 }, { 0x00405900, 0x004059ff },
        { 0x00405a00, 0x00405aff }, { 0x00406000, 0x00406518 },
        { 0x00407800, 0x00407fff }, { 0x00408000, 0x004087ff },
        { 0x00408800, 0x004089ff }, { 0x00408a24, 0x00408a24 },
        { 0x00408a80, 0x00408a84 }, { 0x00418000, 0x00418eff },
        { 0x00418380, 0x004183ff }, { 0x00418400, 0x004185ff },
        { 0x00418600, 0x0041867f }, { 0x00418680, 0x004186ff },
        { 0x00418700, 0x004187ff }, { 0x00418800, 0x0041887f },
        { 0x004188c0, 0x00418af8 }, { 0x00418b00, 0x00418bff },
        { 0x00418d00, 0x00418d7f }, { 0x00418f00, 0x00418fff },
        { 0x00419804, 0x00419900 }, { 0x0041be00, 0x0041bfff },
    };
    for (uint32_t i = 0; i < sizeof(gfx) / sizeof(gfx[0]); i++)
        if (a >= gfx[i][0] && a <= gfx[i][1])
            return 1;
    return 0;
}

static void check_golden_trace(const char* dir) {
    uint32_t nsize;
    void* netc = load_file(dir, GA10B_FW_NET_C, &nsize);
    EXPECT(netc != NULL);
    if (netc == NULL)
        return;

    /* enable_hw (pre-ACR): fifo access, then every non-ctx av pair, all
     * before the FECS start (cpuctl alias) */
    int i_start = shim_find_from(0, GA10B_FECS_CPUCTL_ALIAS, 0x2u);
    int i_acc = shim_find_from(0, GA10B_GR_GPFIFO_CTL,
                               GA10B_GR_GPFIFO_CTL_ENABLE);
    EXPECT(i_start > 0 && i_acc >= 0 && i_acc < i_start);
    /* nvgpu non-MIG order: sw_non_ctx, local compute, local gfx, global
     * compute, global gfx -- each list after the previous one */
    static const uint32_t nonctx[] = {
        GA10B_NETC_SW_NON_CTX_LOAD, GA10B_NETC_NON_CTX_LOCAL_COMPUTE,
        GA10B_NETC_NON_CTX_LOCAL_GFX, GA10B_NETC_NON_CTX_GLOBAL_COMPUTE,
        GA10B_NETC_NON_CTX_GLOBAL_GFX,
    };
    uint32_t nonctx_total = 0;
    int i_list = i_acc;
    for (uint32_t r = 0; r < 5; r++) {
        uint32_t cnt = ga10b_netlist_count(netc, nsize, nonctx[r], 2);
        EXPECT(r == 0 || cnt > 0);  /* sw_non_ctx_load is empty in NETC */
        int i_last = i_list;
        for (uint32_t k = 0; k < cnt; k++) {
            uint32_t a, v;
            EXPECT(ga10b_netlist_av(netc, nsize, nonctx[r], k, &a, &v) == 0);
            int f = shim_find_from((uint32_t)i_list, a, v);
            EXPECT(f >= 0 && f < i_start);
            if (f > i_last)
                i_last = f;
        }
        i_list = i_last;
        nonctx_total += cnt;
    }
    EXPECT(nonctx_total > 0);
    EXPECT(shim_nwr > (uint32_t)i_acc + nonctx_total);

    /* before fifo access: SMC timestamp prod, engine reset, SLCG then
     * BLCG prod lists (first/last entries), GR interrupt enable */
    int i_ts = shim_find_from(0, GA10B_SMCARB_TIMESTAMP_CTRL, 0);
    int i_rst = shim_find_from(0, GA10B_GPCS_GPCCS_ENGINE_RESET_CTL, 0);
    int i_slcg = shim_find_from(0, ga10b_slcg_bus[0].addr,
                                ga10b_slcg_bus[0].prod);
    uint32_t nbh = GA10B_GATE_N(ga10b_blcg_hshub);
    int i_blcg = shim_find_from(0, ga10b_blcg_hshub[nbh - 1u].addr,
                                ga10b_blcg_hshub[nbh - 1u].prod);
    int i_ien = shim_find_from(0, GA10B_GR_INTR_EN, GA10B_GR_INTR_EN_MASK);
    EXPECT(i_ts >= 0 && i_rst > i_ts && i_slcg > i_rst && i_blcg > i_slcg);
    EXPECT(i_ien > i_blcg && i_ien < i_acc);
    for (uint32_t k = 0; k < GA10B_GATE_N(ga10b_slcg_gr); k++)
        EXPECT(shim_find_from((uint32_t)i_slcg, ga10b_slcg_gr[k].addr,
                              ga10b_slcg_gr[k].prod) < i_blcg);
    for (uint32_t k = 0; k < GA10B_GATE_N(ga10b_blcg_gr); k++) {
        int f = shim_find_from((uint32_t)i_slcg, ga10b_blcg_gr[k].addr,
                               ga10b_blcg_gr[k].prod);
        EXPECT(f > i_slcg && f < i_blcg);
    }
    printf("  enable_hw: %u non-ctx pairs written before FECS start\n",
           nonctx_total);

    /* start_gpccs then start_fecs: ctxsw mailbox 0/6 cleared and
     * mailbox 1 = 1 between the two cpuctl writes; the generic falcon
     * mailboxes (0x409040/44) are never used */
    int i_gp = shim_find_from(0, GA10B_FECS_CPUCTL_ALIAS +
                              GA10B_FECS_GPCCS_OFFSET, 0x2u);
    int i_mb1 = shim_find_from(0, GA10B_FECS_CTXSW_MBOX(1), 1u);
    int i_mb6 = shim_find_from(0, GA10B_FECS_CTXSW_MBOX(6), 0u);
    EXPECT(i_gp >= 0 && i_mb1 > i_gp && i_mb6 > i_mb1 && i_start > i_mb6);
    EXPECT(shim_count_off(GA10B_FECS_FALCON_BASE + GA10B_FALCON_MAILBOX0) == 0);
    EXPECT(shim_count_off(GA10B_FECS_FALCON_BASE + GA10B_FALCON_MAILBOX1) == 0);
    /* no GR/GPC reset once the falcons run (it belongs to enable_hw) */
    EXPECT(shim_find_from((uint32_t)i_start,
                          GA10B_GPCS_GPCCS_ENGINE_RESET_CTL, 0) < 0);
    EXPECT(shim_find_from(0, GA10B_GPCS_GPCCS_ENGINE_RESET_CTL, 0) >= 0);

    /* wait_ctxsw_ready tail: current ctx invalidated, watchdog method */
    int i_wdt = shim_find_from((uint32_t)i_start, GA10B_FECS_METHOD_PUSH,
                               GA10B_FECS_MTHD_WATCHDOG);
    EXPECT(i_wdt > i_start);
    EXPECT(shim_trace[i_wdt - 1].off == GA10B_FECS_METHOD_DATA &&
           shim_trace[i_wdt - 1].val == GA10B_FECS_WDT_DEFAULT);
    int i_inv = shim_find_from((uint32_t)i_start, GA10B_FECS_CURRENT_CTX, 0);
    EXPECT(i_inv > i_start && i_inv < i_wdt);

    /* phase anchors: FECS method pushes (method_push = 0x409504) */
    int i_disc = shim_find_from(0, GA10B_FECS_METHOD_PUSH,
                                GA10B_FECS_MTHD_DISCOVER_IMAGE_SIZE);
    int i_bind = shim_find_from(0, GA10B_FECS_METHOD_PUSH,
                                GA10B_FECS_MTHD_BIND_PTR);
    int i_save = shim_find_from(0, GA10B_FECS_METHOD_PUSH,
                                GA10B_FECS_MTHD_WFI_GOLDEN_SAVE);
    /* commit_hw_state opens with fe_go_idle_timeout disable */
    int i_commit = shim_find_from((uint32_t)i_bind,
                                  GA10B_GR_FE_GO_IDLE_TIMEOUT, 0);
    EXPECT(i_disc > i_wdt && i_bind > i_disc && i_commit > i_bind &&
           i_save > i_commit);

    /* setup_hw AFTER the falcons are up (gr_init_support_impl first
     * power-on order), before the golden ctx: non-MIG engine config +
     * GPC MMU mirrors of the FB MMU state (dry rd=0) + FECS host interrupt
     * enable */
    EXPECT(GA10B_GR_ENGINE_CONFIG_ALL == 0xfu);
    EXPECT(shim_count_off(GA10B_GR_ENGINE_CONFIG) == 1);
    int i_cfg = shim_find_from(0, GA10B_GR_ENGINE_CONFIG,
                               GA10B_GR_ENGINE_CONFIG_ALL);
    int i_mmu = shim_find_from(0, GA10B_GR_GPCS_MMU_CTRL, 0);
    int i_hint = shim_find_from(0, GA10B_FECS_HOST_INT_ENABLE,
                                GA10B_FECS_HOST_INT_ALL);
    EXPECT(i_cfg > i_disc && i_mmu > i_cfg && i_hint > i_mmu &&
           i_hint < i_bind);
    EXPECT(shim_trace[i_mmu + 6].off == GA10B_GR_GPCS_MMU_NUM_ACTIVE_LTCS);

    /* sw_ctx_load: every (addr,value) pair written inside the init_hw_state
     * window, graphics ranges included (non-MIG nvgpu applies no filter) */
    uint32_t n = ga10b_netlist_count(netc, nsize, GA10B_NETC_SW_CTX_LOAD, 3);
    uint32_t present = 0, gfx = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t a, idx, v;
        EXPECT(ga10b_netlist_aiv(netc, nsize, GA10B_NETC_SW_CTX_LOAD, k,
                                 &a, &idx, &v) == 0);
        int f = shim_find_from((uint32_t)i_bind, a, v);
        EXPECT(f >= 0 && f < i_commit);
        if (in_gfx(a))
            gfx++;
        else
            present++;
    }
    EXPECT(present + gfx == n);   /* every entry classified */
    EXPECT(present > 100 && gfx > 100);
    printf("  ctx_load: %u compute + %u gfx pairs written (of %u)\n",
           present, gfx, n);

    /* hww masks (dry rd=0 -> sm0 read contributes 0) */
    EXPECT(shim_find_from(0, GA10B_GR_SMS_HWW_GLOBAL_ESR, ~0u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_SMS_HWW_WARP_MASK,
                          GA10B_GR_HWW_WARP_CONST) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_SMS_HWW_GLOBAL_MASK,
                          GA10B_GR_HWW_GLOBAL_CONST) >= 0);

    /* fe power force_on then auto */
    EXPECT(shim_find_from(0, GA10B_GR_FE_PWR_MODE, 0x12u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_FE_PWR_MODE, 0x10u) >= 0);

    /* bundle address-write count: av + av64 + veid expanded x64 */
    uint32_t nb = ga10b_netlist_count(netc, nsize,
                                      GA10B_NETC_SW_BUNDLE_INIT, 2);
    uint32_t nv = ga10b_netlist_count(netc, nsize,
                                      GA10B_NETC_SW_VEID_BUNDLE_INIT, 2);
    uint32_t n64 = ga10b_netlist_count(netc, nsize,
                                       GA10B_NETC_SW_BUNDLE64_INIT, 3);
    uint32_t nm = ga10b_netlist_count(netc, nsize,
                                      GA10B_NETC_SW_METHOD_INIT, 2);
    uint32_t expect_addr = nb + n64;
    for (uint32_t k = 0; k < nv; k++) {
        uint32_t a, v;
        EXPECT(ga10b_netlist_av(netc, nsize, GA10B_NETC_SW_VEID_BUNDLE_INIT,
                                k, &a, &v) == 0);
        expect_addr += ((a & 0xffffu) == GA10B_GR_BUNDLE_GO_IDLE) ?
                       1u : GA10B_GR_MAX_SUBCTX;
    }
    EXPECT(shim_count_off(GA10B_GR_PIPE_BUNDLE_ADDR) == expect_addr);
    printf("  bundle addr writes: %u (av %u + veid %u expanded + 64b %u)\n",
           expect_addr, nb, nv, n64);

    /* pipe override on then off */
    int i_on = shim_find_from(0, GA10B_GR_PIPE_BUNDLE_CFG, 0x80000000u);
    int i_off = shim_find_from(0, GA10B_GR_PIPE_BUNDLE_CFG, 0);
    EXPECT(i_on >= 0 && i_off > i_on);

    /* method init: one shadow-index write per entry, trigger bit set */
    EXPECT(shim_count_off(GA10B_GR_MME_SHADOW_INDEX) == nm);
    uint32_t bad_trig = 0;
    for (uint32_t k = 0; k < shim_nwr; k++)
        if (shim_trace[k].off == GA10B_GR_MME_SHADOW_INDEX &&
            !(shim_trace[k].val & 0x80000000u))
            bad_trig++;
    EXPECT(bad_trig == 0);

    /* timeslice / fs_state RMW results (dry rd=0) */
    EXPECT(shim_find_from(0, GA10B_GR_PE_VAF, 0x10u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_PES_VSC_VPC, 0x4u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_PD_AB_DIST_CFG0, 0x80000000u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_SM_TEXIO_CTRL, 0x100u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_SM_DISP_CTRL, 0x1000u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_DEBUG_0, 0x800u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_MPC_PIX_DEBUG, 0u) >= 0);

    /* dry-run topology = full 2 GPC x 4 TPC, PES masks {0x3, 0xc}.  The
     * gv100 sm-id table (hand-derived from gv100_gr_config_init_sm_id_table:
     * drop the TPC that keeps the best perf, last candidate wins ties)
     * orders the global tpc index as (gpc,tpc) =
     * (1,3) (0,3) (1,1) (0,1) (1,2) (0,2) (1,0) (0,0) */
    static const uint32_t sm_gpc[8] = { 1, 0, 1, 0, 1, 0, 1, 0 };
    static const uint32_t sm_tpc[8] = { 3, 3, 1, 1, 2, 2, 0, 0 };
    EXPECT(shim_find_from(0, GA10B_GR_CWD_FS, 2u | (8u << 8)) >= 0);
    /* early config: live PES mask, then numbering, all before GPCCS start */
    int i_pes = shim_find_from(0, GA10B_GR_GPC0_SWDX_PES_MASK, 0);
    EXPECT(i_pes >= 0 && i_pes < i_gp);
    EXPECT(shim_find_from(0, GA10B_GR_GPC0_SWDX_PES_MASK +
                             GA10B_GPC_STRIDE, 0) < i_gp);
    for (uint32_t k = 0; k < 8; k++) {
        uint32_t cfg = GA10B_GR_GPC0_SM_CFG + sm_gpc[k] * GA10B_GPC_STRIDE +
                       sm_tpc[k] * GA10B_TPC_STRIDE;
        uint32_t pd = GA10B_GR_GPC0_GPM_PD_SM_ID(sm_tpc[k]) +
                      sm_gpc[k] * GA10B_GPC_STRIDE;
        int f = shim_find_from((uint32_t)i_pes, cfg, k);
        EXPECT(f > i_pes && f < i_gp);
        EXPECT(shim_trace[f + 1].off == pd && shim_trace[f + 1].val == k);
        /* one value per register, written per SM early + at golden init */
        EXPECT(shim_count_off(cfg) == 2u * GA10B_SM_PER_TPC);
        for (uint32_t j = 0; j < shim_nwr; j++)
            if (shim_trace[j].off == cfg)
                EXPECT(shim_trace[j].val == k);
    }
    EXPECT(shim_find_from(0, GA10B_GR_PD_NUM_TPC_PER_GPC(0), 0x44u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_DS_NUM_TPC_PER_GPC(0), 0x44u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_CWD_GPC_TPC_ID(0), 0x01110313u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_CWD_GPC_TPC_ID(1), 0x00100212u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_CWD_SM_ID(0), 0x01050307u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_CWD_SM_ID(1), 0x00040206u) >= 0);

    /* auto go idle off then on; timeouts restored; ctx invalidated */
    EXPECT(shim_find_from(0, GA10B_GR_DEBUG_2, 0u) >= 0);
    EXPECT(shim_find_from(0, GA10B_GR_DEBUG_2, 0x60000u) >= 0);
    EXPECT(shim_find_from((uint32_t)i_commit,
                          GA10B_GR_FE_GO_IDLE_TIMEOUT, 0x1800u) >= 0);
    EXPECT(shim_find_from((uint32_t)i_commit,
                          GA10B_GR_FE_COMPUTE_GO_IDLE_TIMEOUT, 0x40u) >= 0);
    EXPECT(shim_find_from((uint32_t)i_save, GA10B_FECS_CURRENT_CTX, 0) >= 0);
    free(netc);
}

/* fifo init on the dry-run runlist placeholder (base 0x800000) */
static void check_fifo_init(void) {
    const uint32_t rl = GA10B_RUNLIST0_BASE;
    /* CG prods land inside the runlist aperture, never at BAR0 0x50/0x54 */
    EXPECT(shim_find_from(0, rl + GA10B_RL_SLCG, 0) >= 0);
    EXPECT(shim_find_from(0, rl + GA10B_RL_BLCG, 0x4042u) >= 0);
    EXPECT(shim_count_off(0x50u) == 0 && shim_count_off(0x54u) == 0);
    EXPECT(shim_find_from(0, rl + GA10B_RL_VIRT_CH_CFG(0), 0x800007ffu) >= 0);
    EXPECT(shim_find_from(0, rl + GA10B_RL_USERD_WRITEBACK, 0x64u) >= 0);
    /* nvgpu never touches sched_disable at init/submit; ELPG is BPMP's */
    EXPECT(shim_count_off(rl + GA10B_RL_SCHED_DISABLE) == 0);
    EXPECT(shim_count_off(0x20cu) == 0);
}

/* runlist + CHRAM traffic of channel create/destroy (dry chram base 0) */
static void check_runlist(void) {
    const uint32_t rl = GA10B_RUNLIST0_BASE;
    uint32_t prev_len = 0, nsub = 0;
    int i_first_sub = -1;
    for (uint32_t k = 0; k < shim_nwr; k++) {
        if (shim_trace[k].off != rl + GA10B_RL_SUBMIT)
            continue;
        if (i_first_sub < 0)
            i_first_sub = (int)k;
        /* each create/destroy moves the length by exactly one [TSG, CH] */
        uint32_t len = shim_trace[k].val;
        EXPECT(len == prev_len + 2u || len + 2u == prev_len);
        prev_len = len;
        nsub++;
    }
    EXPECT(nsub == 8);               /* 4 creates + 4 destroys */
    EXPECT(prev_len == 0);           /* last runlist is empty */
    /* channel 0: runlist first, then CHRAM enable (bind) */
    int i_en = shim_find_from(0, GA10B_CHRAM_CHANNEL(0), GA10B_CHRAM_ENABLE);
    EXPECT(i_first_sub >= 0 && i_en > i_first_sub);
    /* teardown: disable, clear, then the runlist without the channel */
    int i_dis = shim_find_from(0, GA10B_CHRAM_CHANNEL(0), GA10B_CHRAM_DISABLE);
    int i_clr = shim_find_from(0, GA10B_CHRAM_CHANNEL(0), GA10B_CHRAM_CLEAR);
    EXPECT(i_dis > i_en && i_clr == i_dis + 1);
    EXPECT(shim_trace[i_clr + 1].off == rl + GA10B_RL_SUBMIT_BASE_LO ||
           shim_trace[i_clr + 1].off == rl + GA10B_RL_SUBMIT);
    /* TLB invalidate: all_va | trigger, never all_pdb */
    EXPECT(shim_count_off(GA10B_FB_MMU_INVALIDATE) > 0);
    for (uint32_t k = 0; k < shim_nwr; k++)
        if (shim_trace[k].off == GA10B_FB_MMU_INVALIDATE)
            EXPECT(shim_trace[k].val == 0x80000001u);
}

/* last submitted runlist must list both channels as [TSG, CH] pairs */
static void check_live_runlist(const ga10b_chan_t* a, const ga10b_chan_t* b) {
    const uint32_t rl = GA10B_RUNLIST0_BASE;
    int lo = -1, hi = -1, sub = -1;
    for (uint32_t k = 0; k < shim_nwr; k++) {
        if (shim_trace[k].off == rl + GA10B_RL_SUBMIT_BASE_LO) lo = (int)k;
        if (shim_trace[k].off == rl + GA10B_RL_SUBMIT_BASE_HI) hi = (int)k;
        if (shim_trace[k].off == rl + GA10B_RL_SUBMIT) sub = (int)k;
    }
    EXPECT(lo >= 0 && hi == lo + 1 && sub == hi + 1);
    if (lo < 0 || hi < 0 || sub < 0)
        return;
    EXPECT(shim_trace[sub].val == 4u);          /* offset 0, 4 entries */
    EXPECT((shim_trace[lo].val & 3u) == GA10B_RL_TARGET_SYS_NCOH);
    uint64_t phy = ((uint64_t)shim_trace[hi].val << 32) |
                   (shim_trace[lo].val & ~0x3ffu);
    uint32_t* e = (uint32_t*)shim_dma_va(phy);
    EXPECT(e != NULL);
    if (e == NULL)
        return;
    uint32_t want[16];
    ga10b_rl_tsg(want, a->tsgid, 1);
    ga10b_rl_ch(want + 4, a->chid, a->inst_phy, a->userd_phy);
    ga10b_rl_tsg(want + 8, b->tsgid, 1);
    ga10b_rl_ch(want + 12, b->chid, b->inst_phy, b->userd_phy);
    EXPECT(memcmp(e, want, sizeof(want)) == 0);
    EXPECT(e[0] == 0x80030001u);                /* non-zero timeslice */
}

static void check_channel(ga10b_chan_t* ch) {
    uint32_t* inst = (uint32_t*)ch->inst_va;
    /* RAMFC (fifo layer) */
    EXPECT(inst[GA10B_RAMFC_SIGNATURE] == GA10B_CLASS_CHANNEL_GPFIFO_B);
    EXPECT(inst[GA10B_RAMFC_ACQUIRE] == 0x102u);
    EXPECT(inst[GA10B_RAMFC_PB_HEADER] == GA10B_RAMFC_PB_HEADER_VAL);
    EXPECT(inst[GA10B_RAMFC_SUBDEVICE] == GA10B_RAMFC_SUBDEVICE_VAL);
    EXPECT(inst[GA10B_RAMFC_CONFIG] == GA10B_RAMFC_CONFIG_VAL);
    EXPECT(inst[GA10B_RAMIN_FW_MAGIC] == GA10B_RAMIN_FW_MAGIC_VAL);
    /* RAMIN GR (ctx_prepare) */
    EXPECT(inst[132] == (4u | (uint32_t)(((ch->gr_hdr_gpu_va >> 12) &
                                          0xfffffu) << 12)));
    EXPECT(inst[133] == (uint32_t)(ch->gr_hdr_gpu_va >> 32));
    EXPECT(inst[166] == 1u);
    EXPECT(inst[168] == inst[128]);
    EXPECT(inst[169] == inst[129]);
    /* main ctx header patches */
    uint32_t* c = (uint32_t*)ch->gr_ctx_va;
    EXPECT(c[GA10B_CTX_W_ACCESS_MAP_CFG] == GA10B_CTX_ACCESS_MAP_USE_MAP);
    EXPECT(c[GA10B_CTX_W_COMPUTE_PREEMPT] == GA10B_CTX_COMPUTE_PREEMPT_CTA);
    EXPECT(!(c[GA10B_CTX_W_MISC_OPTIONS] & 8u));
    EXPECT(c[GA10B_CTX_W_NUM_WFI_SAVE] == 0);
    EXPECT(c[GA10B_CTX_W_PATCH_ADR_LO] == (uint32_t)ch->gr_patch_gpu_va);
    /* veid header page */
    uint32_t* h = (uint32_t*)ch->gr_hdr_va;
    EXPECT(h[GA10B_CTX_W_CTL] == GA10B_CTX_CTL_PER_VEID);
    EXPECT(h[GA10B_CTX_W_CTX_BUF_PTR] == (uint32_t)ch->gr_ctx_gpu_va);
    EXPECT(h[GA10B_CTX_W_CTX_BUF_PTR_HI] ==
           (uint32_t)(ch->gr_ctx_gpu_va >> 32));
    /* dispatch pushbuffer: SET_OBJECT compute class */
    uint32_t* pb = (uint32_t*)ch->pb_va;
    EXPECT(pb[0] == ga10b_pb_inc(GA10B_SUBCH_COMPUTE, GA10B_MTHD_SET_OBJECT, 1));
    EXPECT(pb[1] == GA10B_CLASS_COMPUTE_B);
    /* GPFIFO entry length = 13 dwords, USERD gp_put advanced */
    uint32_t* e = (uint32_t*)ch->gpfifo_va;
    EXPECT((e[1] >> 10) == 13u);
    EXPECT(((uint32_t*)ch->userd_va)[GA10B_USERD_GP_PUT] == ch->gpfifo_put);
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "/Users/tony/work/gpu-fw/ga10b";
    uint32_t nsize;
    void* probe = load_file(dir, GA10B_FW_NET_C, &nsize);
    if (probe == NULL) {
        printf("SKIP: firmware dir %s has no NETC blob\n", dir);
        return 0;
    }
    free(probe);

    ga10b.hw = 0;
    shim_quiet = 1;

    /* ---- stage order of ga10bd.c: fifo -> gr enable_hw -> (ACR) -> gr */
    EXPECT(ga10b_gr_init(dir) == -1);           /* enable_hw is mandatory */
    EXPECT(ga10b_fifo_init() == 0);
    check_fifo_init();
    EXPECT(ga10b_gr_enable_hw(dir) == 0);
    /* ---- M1: full golden-ctx init through the real code ---- */
    EXPECT(ga10b_gr_init(dir) == 0);
    check_golden_trace(dir);

    /* ---- channel + compute dispatch ---- */
    uint32_t live_after_init = shim_dma_live;   /* leak baseline */
    ga10b_gmmu_t mm;
    EXPECT(ga10b_gmmu_init(&mm) == 0);
    ga10b_chan_t ch;
    EXPECT(ga10b_chan_create(&mm, &ch, GA10B_DEVTYPE_GRAPHICS, 0) == 0);
    EXPECT(ga10b_compute_dispatch(&ch, 0) == 0);
    check_channel(&ch);
    /* doorbell rung with the channel token */
    EXPECT(shim_count_off(GA10B_DOORBELL) >= 1);

    /* second dispatch: ctx_prepare idempotent, ring advances */
    void* ctx0 = ch.gr_ctx_va;
    EXPECT(ga10b_compute_dispatch(&ch, 0) == 0);
    EXPECT(ch.gr_ctx_va == ctx0);
    EXPECT(((uint32_t*)ch.userd_va)[GA10B_USERD_GP_PUT] == 2u);

    /* two channels live at once: the runlist carries both [TSG, CH]
     * pairs, in the buffer the last submit pointed at */
    ga10b_chan_t ch2;
    EXPECT(ga10b_chan_create(&mm, &ch2, GA10B_DEVTYPE_GRAPHICS, 0) == 0);
    EXPECT(ch2.chid != ch.chid && ch2.tsgid != ch.tsgid);
    check_live_runlist(&ch, &ch2);

    /* hw-mode guard: RAMIN GR setup must precede the first submit */
    EXPECT(ga10b_chan_bind_engine(&ch2, 0, GA10B_CLASS_COMPUTE_B) == 0);
    ga10b.hw = 1;               /* bar0 stays NULL: rd/wr still dry-log */
    EXPECT(ga10b_gr_ctx_prepare(&ch2) == -1);
    ga10b.hw = 0;
    ga10b_chan_destroy(&ch2);

    /* ---- M0 regression: CE blit through the same channel layer ---- */
    EXPECT(ga10b_ce_init() == 0);
    ga10b_chan_t ch3;
    EXPECT(ga10b_chan_create(&mm, &ch3, GA10B_DEVTYPE_GRAPHICS, 0) == 0);
    void *src, *dst;
    uint64_t src_phy, dst_phy;
    EXPECT(ga10b_dma_alloc(4096, 4096, &src, &src_phy) == 0);
    EXPECT(ga10b_dma_alloc(4096, 4096, &dst, &dst_phy) == 0);
    uint64_t src_va = ga10b_gmmu_map(&mm, src_phy, 4096);
    uint64_t dst_va = ga10b_gmmu_map(&mm, dst_phy, 4096);
    EXPECT(src_va != 0 && dst_va != 0);
    EXPECT(ga10b_ce_blit(&ch3, src_va, 256, dst_va, 256, 256, 4) == 0);
    uint32_t* pb3 = (uint32_t*)ch3.pb_va;
    int launch = 0;
    for (uint32_t k = 0; k < ch3.pb_put; k++)
        if (pb3[k] == 0x200180c0u)      /* ce_app.c LAUNCH_DMA, subch 4 */
            launch = 1;
    EXPECT(launch);
    EXPECT(pb3[0] == 0x20018000u);      /* ce_app.c SET_OBJECT, subch 4 */
    EXPECT(pb3[1] == GA10B_CLASS_DMA_COPY_B);
    /* CE interrupt mask = ce_lce_intr_mask (no nonstalling-debug bit 30) */
    EXPECT(shim_find_from(0, GA10B_CE_LCE_INTR_EN(0), 0x8000007fu) >= 0);
    EXPECT(shim_find_from(0, GA10B_CE_LCE_INTR_EN(1), 0x8000007fu) >= 0);
    /* runqueue translation: bar2 host VM bound (virtual mode + sys_ncoh).
     * Without this PBDMA fetches GPFIFO but never posts methods. */
    EXPECT(shim_count_off(GA10B_BUS_BAR2_BLOCK) >= 1);
    int bar2_virtual = 0;
    for (uint32_t k = 0; k < shim_nwr; k++)
        if (shim_trace[k].off == GA10B_BUS_BAR2_BLOCK &&
            (shim_trace[k].val & GA10B_BUS_BAR2_MODE_VIRTUAL))
            bar2_virtual = 1;
    EXPECT(bar2_virtual);
    EXPECT(ga10b_fifo_bar2_ntab() > 0);
    /* stalling tree -> CPU, nonstalling tree off (nvgpu Linux build) */
    EXPECT(shim_find_from(0, GA10B_CE_LCE_INTR_CTRL(0), 0x80000000u) >= 0);
    EXPECT(shim_find_from(0, GA10B_CE_LCE_INTR_NOTIFY_CTRL(0), 0) >= 0);
    EXPECT(shim_find_from(0, GA10B_CE_LCE_INTR_NOTIFY_CTRL(0),
                          0x80000000u) < 0);

    /* a fourth channel exercises create-while-others-live once more */
    ga10b_chan_t ch4;
    EXPECT(ga10b_chan_create(&mm, &ch4, GA10B_DEVTYPE_GRAPHICS, 0) == 0);
    ga10b_chan_destroy(&ch4);

    ga10b_chan_destroy(&ch3);
    ga10b_chan_destroy(&ch);
    check_runlist();
    ga10b_gmmu_destroy(&mm);
    ga10b_dma_free(src);
    ga10b_dma_free(dst);
    /* gr-global surfaces are retained by design (no gr teardown): the
     * golden set (baseline) + the qmd/local dispatch buffers allocated on
     * first dispatch + the bar2 host-VM page-table chain (runqueue
     * translation, allocated on first channel, lives for driver lifetime).
     * Everything channel/CE/mm-side must be freed. */
    uint32_t bar2_tab = ga10b_fifo_bar2_ntab();
    printf("  dma live after teardown: %u (baseline %u + 2 dispatch + %u bar2)\n",
           shim_dma_live, live_after_init, bar2_tab);
    EXPECT(shim_dma_live == live_after_init + 2 + bar2_tab);

    if (fails == 0)
        printf("test_integration: ALL PASS (%u MMIO writes traced)\n",
               shim_nwr);
    else
        printf("test_integration: %d FAILURES\n", fails);
    return fails != 0;
}