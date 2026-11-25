/*
 * GR / compute (M1) for GA10B.
 *
 * Bring-up order transcribed from nvgpu rel-36 (common/init/nvgpu_init.c
 * nvgpu_finalize_poweron, common/gr/gr.c):
 *   ga10b_gr_enable_hw, BEFORE the ACR runs (nvgpu_gr_enable_hw):
 *     GR engine disable -> GPC reset -> engine enable -> SLCG/BLCG prods ->
 *     GR interrupts -> fifo access -> sw_non_ctx_load + non-ctx compute AND
 *     graphics lists -> falcon scrub -> idle
 *   ACR-GSP authenticates FECS/GPCCS into WPR (ga10b_acr.c)
 *   ga10b_gr_init (gr_init_support_impl, first power-on order):
 *     GPC/TPC/PES discovery -> live PES mask -> PES-aware sm-id numbering
 *     -> start gpccs/fecs -> ctxsw handshake -> current ctx invalid -> FECS
 *     watchdog -> DISCOVER_IMAGE_SIZE -> engine config -> GPC MMU -> golden
 *     context (netlist-driven, common/gr/obj_ctx.c).
 * Resetting the GR engine after the ACR ran would throw away the LS
 * falcon state the ACR just set up, hence the split.
 *
 * Golden context is COMPUTE-ONLY: graphics global buffers (pagepool/
 * bundle_cb/attrib_cb) are skipped, only the 512KB priv access map is
 * needed.  NOTE: this is a hybrid no nvgpu build runs as-is -- default
 * (non-MIG) nvgpu always commits the graphics buffers, while its MIG mode
 * additionally filters bundles/methods, skips the global timeslice and
 * tells the ACR about it.  Which subset real hardware accepts is an open
 * hardware-validation item.  Flow: dedicated VM + RAMIN-only
 * inst block (subctx0 pdb = main pdb) -> header page -> BIND_PTR ->
 * sw_ctx_load writel list (gfx regs filtered) -> hww masks -> timeslice +
 * floorsweep/sm-id -> bundle/veid-bundle/bundle64 init -> mme method init
 * -> GOLDEN_IMAGE_SAVE -> CPU copy to local golden.  Per-channel load =
 * memcpy + main-header patches + veid header page (ga10b_gr_ctx_prepare).
 *
 * All MMIO goes through ga10b_rd/wr/poll: without --hw the whole sequence
 * executes as a logged dry-run; hardware requires the explicit CLI flag.
 */

#include <stdlib.h>
#include <string.h>

#include <ewoksys/klog.h>
#include <ewoksys/kernel_tic.h>

#include "ga10b.h"
#include "ga10b_regs.h"
#include "ga10b_fifo.h"
#include "ga10b_netlist.h"
#include "ga10b_ctx.h"
#include "ga10b_gating.h"

#define GR_CTX_IMAGE_FALLBACK  0x80000u   /* dry-run DISCOVER placeholder */
#define GR_LOCAL_MEM_SIZE      0x10000u   /* 64KB shader local window */
#define GR_DISPATCH_WORDS      13u
#define GR_SEM_TRIES           200000u    /* ~2s at 10us */
#define GR_QMD_REGS            32u
#define GR_IDLE_TRIES          100000u
#define GR_SEM_SPIN_NS         4000000ull /* busy-wait before sleeping */
#define FECS_METHOD_TRIES      300000u    /* ~3s at 10us (nvgpu poll timeout) */
#define GR_GPCS_RESET_DELAY_US 20u        /* NVGPU_GR_GPCS_RESET_DELAY_US */
#define GR_MAX_TPC             (GA10B_MAX_GPC * GA10B_MAX_TPC_PER_GPC)

static struct {
    int      ready;
    int      hw_enabled;
    uint32_t sm_total;
    uint32_t ctx_image_size;
    /* topology: logical (floorsweep-compacted) gpc ids */
    uint32_t gpc_count;
    uint32_t tpc_total;
    uint32_t gpc_tpc_mask[GA10B_MAX_GPC];
    uint32_t gpc_tpc_count[GA10B_MAX_GPC];
    /* PES layout (gr_config_init_pes_tpc) */
    uint32_t pes_tpc_mask[GA10B_PES_PER_GPC][GA10B_MAX_GPC];
    uint32_t gpc_ppc_count[GA10B_MAX_GPC];
    uint32_t ppc_count;
    /* sm-id table: global tpc index -> logical (gpc, tpc) */
    uint32_t sm_gpc[GR_MAX_TPC];
    uint32_t sm_tpc[GR_MAX_TPC];
    int      sm_identity;        /* plain tpc order, no PES remap */
    void*    golden_va;
    uint64_t golden_phy;
    /* golden-init surfaces (kept alive; local copy feeds channels) */
    void*    golden_local;
    ga10b_gmmu_t golden_mm;
    void*    g_inst_va;  uint64_t g_inst_phy;
    void*    g_hdr_va;   uint64_t g_hdr_phy;   uint64_t g_hdr_gpu_va;
    void*    g_patch_va; uint64_t g_patch_phy; uint64_t g_patch_gpu_va;
    void*    g_map_va;   uint64_t g_map_phy;   uint64_t g_map_gpu_va;
    uint64_t g_ctx_gpu_va;
    /* graphics global ctx buffers (pagepool, bundle cb, attrib cb, rtv cb) */
    uint32_t max_tpc;
    struct { void* va; uint64_t phy; uint32_t size; } gbuf[4];
    uint64_t g_gbuf_va[4];
    /* dispatch buffers, allocated on first use (single-channel skeleton) */
    ga10b_gmmu_t* buf_mm;
    void*    qmd_va;   uint64_t qmd_phy;   uint64_t qmd_gpu_va;
    void*    local_va; uint64_t local_phy; uint64_t local_gpu_va;
    ga10b_fw_t netc;
} gr;

/* FECS host-method protocol (gm20b_gr_falcon_submit_fecs_method_op +
 * gm20b_gr_falcon_ctx_wait_ucode).  Each method has its own mailbox(0)
 * clear mask and pass/fail conditions (gm20b/ga100 ctrl_ctxsw tables);
 * ga10b clears mailbox bits by read-modify-write of the mailbox itself
 * (ga10b_gr_falcon_fecs_ctxsw_clear_mailbox). */
enum { FECS_OP_SKIP, FECS_OP_AND, FECS_OP_NOT_EQUAL };

typedef struct {
    uint32_t adr;
    uint32_t clr;                /* mailbox(0) bits cleared before push */
    uint32_t ok_op, ok;
    uint32_t fail_op, fail;
} fecs_op_t;

static const fecs_op_t FECS_BIND_PTR =
    { GA10B_FECS_MTHD_BIND_PTR, 0x30u, FECS_OP_AND, 0x10u, FECS_OP_AND, 0x20u };
static const fecs_op_t FECS_GOLDEN_SAVE =
    { GA10B_FECS_MTHD_WFI_GOLDEN_SAVE, 0x3u, FECS_OP_AND, 0x1u,
      FECS_OP_AND, 0x2u };
static const fecs_op_t FECS_DISCOVER_IMAGE_SIZE =
    { GA10B_FECS_MTHD_DISCOVER_IMAGE_SIZE, ~0u, FECS_OP_NOT_EQUAL, 0u,
      FECS_OP_SKIP, 0u };
static const fecs_op_t FECS_SET_WATCHDOG =
    { GA10B_FECS_MTHD_WATCHDOG, ~0u, FECS_OP_SKIP, 0u, FECS_OP_SKIP, 0u };

static void fecs_mbox_clear(uint32_t idx, uint32_t bits) {
    ga10b_wr(GA10B_FECS_CTXSW_MBOX(idx),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(idx)) & ~bits);
}

static int fecs_cond(uint32_t op, uint32_t reg, uint32_t val) {
    if (op == FECS_OP_AND)
        return (reg & val) != 0;
    if (op == FECS_OP_NOT_EQUAL)
        return reg != val;
    return 0;
}

/* ret (optional) receives the final mailbox(0) value */
static int fecs_method(const fecs_op_t* op, uint32_t data, uint32_t* ret) {
    fecs_mbox_clear(0, op->clr);
    ga10b_wr(GA10B_FECS_METHOD_DATA, data);
    ga10b_wr(GA10B_FECS_METHOD_PUSH, op->adr & 0xfffu);
    if (ret != NULL)
        *ret = 0;
    if (op->ok_op == FECS_OP_SKIP && op->fail_op == FECS_OP_SKIP)
        return 0;
    if (!ga10b.hw)
        return 0;                /* dry-run: nothing answers */
    uint32_t reg = 0;
    for (uint32_t i = 0; i < FECS_METHOD_TRIES; i++) {
        reg = ga10b_rd(GA10B_FECS_CTXSW_MBOX(0));
        if (fecs_cond(op->fail_op, reg, op->fail)) {
            klog("ga10b: fecs method %02x FAILED (mbox0=%08x)\n", op->adr,
                 reg);
            return -1;
        }
        if (fecs_cond(op->ok_op, reg, op->ok)) {
            if (ret != NULL)
                *ret = reg;
            return 0;
        }
        ga10b_delay_us(10);
    }
    klog("ga10b: fecs method %02x timeout (mbox0=%08x)\n", op->adr, reg);
    return -1;
}

static uint32_t popcount32(uint32_t v) {
    uint32_t n = 0;
    while (v) { v &= v - 1u; n++; }
    return n;
}

/* gr_config_init_pes_tpc: which TPCs hang off each PES of a GPC.  A PES
 * counts as present only when TPCs are connected to it. */
static void gr_pes_config(void) {
    gr.ppc_count = 0;
    for (uint32_t g = 0; g < gr.gpc_count; g++) {
        gr.gpc_ppc_count[g] = 0;
        for (uint32_t p = 0; p < GA10B_PES_PER_GPC; p++) {
            uint32_t m = ga10b_rd(GA10B_GR_GPC0_PES_TPC_ID_MASK(p) +
                                  g * GA10B_GPC_STRIDE) & 0xffu;
            if (!ga10b.hw)             /* dry-run: 2 TPCs per PES */
                m = 0x3u << (2u * p);
            gr.pes_tpc_mask[p][g] = m;
            if (m != 0)
                gr.gpc_ppc_count[g]++;
        }
        gr.ppc_count += gr.gpc_ppc_count[g];
        klog("ga10b: gpc%u pes_tpc_mask=%x,%x ppc=%u\n", g,
             gr.pes_tpc_mask[0][g], gr.pes_tpc_mask[1][g],
             gr.gpc_ppc_count[g]);
    }
}

/* gr_gv100_scg_estimate_perf: fx23.9 score of the config left after taking
 * tpc `dt` of gpc `dg` out of `cand` -- pixel throughput of the weakest GPC
 * [29:20], surviving PES share [19:10], TPC balance across GPCs [9:0] */
static uint32_t gr_scg_perf(const uint32_t* cand, uint32_t dg, uint32_t dt) {
    const uint32_t scale = 512u;
    uint32_t num_tpc[GA10B_MAX_GPC];
    uint32_t min_pix = scale, avg = 0, max_tpc = 0, num_pes = 0;

    for (uint32_t g = 0; g < gr.gpc_count; g++) {
        uint32_t m = cand[g];
        if (g == dg)
            m &= ~(1u << dt);
        num_tpc[g] = popcount32(m);
        avg += num_tpc[g];
        if (num_tpc[g] > max_tpc)
            max_tpc = num_tpc[g];
        uint32_t pix = scale * num_tpc[g] / gr.gpc_tpc_count[g];
        if (pix < min_pix)
            min_pix = pix;
        for (uint32_t p = 0; p < gr.gpc_ppc_count[g]; p++) {
            uint32_t pm = gr.pes_tpc_mask[p][g] & cand[g];
            if (g == dg)
                pm &= ~(1u << dt);
            if (pm != 0)
                num_pes++;
        }
    }
    if (max_tpc == 0)
        return 0;
    avg = scale * avg / gr.gpc_count;
    uint32_t dev = 0;
    for (uint32_t g = 0; g < gr.gpc_count; g++) {
        uint32_t t = scale * num_tpc[g];
        dev += avg > t ? avg - t : t - avg;
    }
    dev /= gr.gpc_count;
    return ((min_pix) << 20) + ((scale * num_pes / gr.ppc_count) << 10) +
           (scale - dev / max_tpc);
}

/* gv100_gr_config_init_sm_id_table: global tpc index order = repeatedly
 * take out the TPC whose removal keeps the best estimated perf (ties: the
 * last candidate wins).  This is NOT the identity order. */
static void gr_sm_id_table(void) {
    uint32_t cand[GA10B_MAX_GPC];
    uint32_t any = 0;
    for (uint32_t g = 0; g < gr.gpc_count; g++) {
        cand[g] = 0;
        for (uint32_t p = 0; p < gr.gpc_ppc_count[g]; p++)
            cand[g] |= gr.pes_tpc_mask[p][g];
        any |= cand[g];
    }
    gr.sm_identity = (ga10b.skip & GA10B_SKIP_SMID_TABLE) != 0 || any == 0;
    if (gr.sm_identity) {
        uint32_t gtpc = 0;
        if (any == 0)
            klog("ga10b: PES tpc masks read 0, identity sm-id order\n");
        for (uint32_t g = 0; g < gr.gpc_count; g++)
            for (uint32_t t = 0; t < GA10B_MAX_TPC_PER_GPC; t++)
                if (gr.gpc_tpc_mask[g] & (1u << t)) {
                    gr.sm_gpc[gtpc] = g;
                    gr.sm_tpc[gtpc] = t;
                    gtpc++;
                }
        return;
    }
    for (uint32_t gtpc = 0; gtpc < gr.tpc_total; gtpc++) {
        uint32_t maxperf = 0;
        gr.sm_gpc[gtpc] = 0;
        gr.sm_tpc[gtpc] = 0;
        for (uint32_t g = 0; g < gr.gpc_count; g++)
            for (uint32_t t = 0; t < gr.gpc_tpc_count[g]; t++) {
                if (!(cand[g] & (1u << t)))
                    continue;
                uint32_t perf = gr_scg_perf(cand, g, t);
                if (perf >= maxperf) {
                    maxperf = perf;
                    gr.sm_gpc[gtpc] = g;
                    gr.sm_tpc[gtpc] = t;
                }
            }
        cand[gr.sm_gpc[gtpc]] &= ~(1u << gr.sm_tpc[gtpc]);
        klog("ga10b: sm-id table gtpc%u = gpc%u tpc%u\n", gtpc,
             gr.sm_gpc[gtpc], gr.sm_tpc[gtpc]);
    }
}

/* gv11b_gr_init_get_nonpes_aware_tpc: TPC index counted PES by PES */
static uint32_t gr_nonpes_tpc(uint32_t g, uint32_t t) {
    if (gr.sm_identity)
        return t;
    uint32_t n = 0, p;
    for (p = 0; p < gr.gpc_ppc_count[g]; p++) {
        if (gr.pes_tpc_mask[p][g] & (1u << t))
            break;
        n += popcount32(gr.pes_tpc_mask[p][g]);
    }
    uint32_t m = p < GA10B_PES_PER_GPC ? gr.pes_tpc_mask[p][g] : 0u;
    return n + popcount32(((1u << t) - 1u) & m);
}

/* gv11b_gr_config_set_live_pes_mask (errata 200075440): every GPC gets the
 * combined live-PES mask, 4 bits per GPC */
static void gr_live_pes_mask(void) {
    uint32_t pes_mask = 0;
    for (uint32_t g = 0; g < gr.gpc_count; g++)
        pes_mask |= (ga10b_rd(GA10B_GR_GPC0_LIVE_PHYS_PES +
                              g * GA10B_GPC_STRIDE) & 0xfu)
                    << (GA10B_PES_MASK_BITS * g);
    for (uint32_t g = 0; g < gr.gpc_count; g++)
        ga10b_wr(GA10B_GR_GPC0_SWDX_PES_MASK + g * GA10B_GPC_STRIDE,
                 pes_mask);
    klog("ga10b: live pes mask %x\n", pes_mask);
}

static void gr_topology(void) {
    uint32_t ngpc = ga10b_rd(GA10B_TOP_NUM_GPCS);
    uint32_t ntpc = ga10b_rd(GA10B_TOP_TPC_PER_GPC);
    if (ngpc == 0 || ngpc > GA10B_MAX_GPC)
        ngpc = GA10B_MAX_GPC;          /* dry-run: TOP reads 0 */
    if (ntpc == 0 || ntpc > GA10B_MAX_TPC_PER_GPC)
        ntpc = GA10B_MAX_TPC_PER_GPC;
    uint32_t fuse_gpc = ga10b_rd(GA10B_FUSE_STATUS_OPT_GPC);

    gr.max_tpc = ngpc * ntpc;
    gr.gpc_count = 0;
    gr.tpc_total = 0;
    gr.sm_total = 0;
    for (uint32_t i = 0; i < ngpc; i++) {
        if (fuse_gpc & (1u << i))
            continue;                  /* floorswept GPC */
        uint32_t fuse_tpc = ga10b_rd(GA10B_FUSE_STATUS_OPT_TPC_GPC(i));
        uint32_t mask = 0;
        for (uint32_t t = 0; t < ntpc; t++)
            if (!(fuse_tpc & (1u << t)))
                mask |= 1u << t;
        uint32_t fs = ga10b_rd(GA10B_GR_GPC0_FS + i * GA10B_GPC_STRIDE);
        uint32_t avail = fs & 0x1fu;   /* num_available_tpcs */
        if (ga10b.hw && avail != 0)
            while (popcount32(mask) > avail)
                mask &= mask - 1u;
        uint32_t live = popcount32(mask);
        if (live == 0)
            continue;
        gr.gpc_tpc_mask[gr.gpc_count] = mask;
        gr.gpc_tpc_count[gr.gpc_count] = live;
        gr.gpc_count++;
        gr.tpc_total += live;
        gr.sm_total += live * GA10B_SM_PER_TPC;
        klog("ga10b: gpc%u live_tpc=%u mask=%x\n", i, live, mask);
    }
    if (gr.sm_total == 0) {            /* dry-run fallback: full config */
        gr.gpc_count = GA10B_MAX_GPC;
        gr.tpc_total = GA10B_MAX_GPC * GA10B_MAX_TPC_PER_GPC;
        gr.sm_total = gr.tpc_total * GA10B_SM_PER_TPC;
        for (uint32_t i = 0; i < GA10B_MAX_GPC; i++) {
            gr.gpc_tpc_mask[i] = (1u << GA10B_MAX_TPC_PER_GPC) - 1u;
            gr.gpc_tpc_count[i] = GA10B_MAX_TPC_PER_GPC;
        }
    }
    klog("ga10b: gr topology gpc=%u tpc=%u sm_total=%u\n",
         gr.gpc_count, gr.tpc_total, gr.sm_total);
    gr_pes_config();
    gr_sm_id_table();
}

/* ga10b_gr_init_override_context_reset (gr_init_ga10b_fusa.c:45-77):
 * raw register values of the assert step, then the deassert step. */
static void gr_override_context_reset(void) {
    ga10b_wr(GA10B_FECS_CTXSW_RESET_CTL, 0x10);
    ga10b_wr(GA10B_GPCCS_CTXSW_RESET_CTL, 0x20);
    ga10b_delay_us(10);
    (void)ga10b_rd(GA10B_FECS_CTXSW_RESET_CTL);
    (void)ga10b_rd(GA10B_GPCCS_CTXSW_RESET_CTL);
    ga10b_wr(GA10B_FECS_CTXSW_RESET_CTL, 0x110);
    ga10b_wr(GA10B_GPCCS_CTXSW_RESET_CTL, 0xa20);
    ga10b_delay_us(10);
    (void)ga10b_rd(GA10B_FECS_CTXSW_RESET_CTL);
    (void)ga10b_rd(GA10B_GPCCS_CTXSW_RESET_CTL);
}

/* ga10b_gr_init_reset_gpcs: gpc_engine_reset enabled (0), 20 us,
 * disabled (1), then every GPC must read back "disabled" */
static int gr_reset_gpcs(void) {
    ga10b_wr(GA10B_GPCS_GPCCS_ENGINE_RESET_CTL, 0);
    ga10b_delay_us(GR_GPCS_RESET_DELAY_US);
    (void)ga10b_rd(GA10B_GPCS_GPCCS_ENGINE_RESET_CTL);
    ga10b_wr(GA10B_GPCS_GPCCS_ENGINE_RESET_CTL, 1);
    (void)ga10b_rd(GA10B_GPCS_GPCCS_ENGINE_RESET_CTL);
    if (ga10b.hw) {
        uint32_t ngpc = ga10b_rd(GA10B_TOP_NUM_GPCS);
        if (ngpc == 0 || ngpc > GA10B_MAX_GPC)
            ngpc = GA10B_MAX_GPC;
        uint32_t fuse_gpc = ga10b_rd(GA10B_FUSE_STATUS_OPT_GPC);
        for (uint32_t i = 0; i < ngpc; i++) {
            if (fuse_gpc & (1u << i))
                continue;              /* floorswept GPC: no reset readback */
            uint32_t r = ga10b_rd(GA10B_GPC0_GPCCS_ENGINE_RESET_CTL +
                                  i * GA10B_GPC_STRIDE);
            if ((r & 1u) != 1u) {
                klog("ga10b: gpc%u not out of reset (%08x)\n", i, r);
                return -1;
            }
        }
    }
    return 0;
}

/* ---- golden-context init (common/gr/obj_ctx.c, compute-only path) ---- */

static int gr_wait_idle(void) {
    /* ga10b_gr_init_wait_idle: gr_status state bit0 only */
    return ga10b_poll(GA10B_GR_STATUS, 1u, 0, GR_IDLE_TRIES);
}

static int gr_wait_fe_idle(void) {
    /* gm20b_gr_init_wait_fe_idle: fe_method_lower = gr_status bit2 */
    return ga10b_poll(GA10B_GR_STATUS, 4u, 0, GR_IDLE_TRIES);
}

static int gr_fe_pwr(int force_on) {
    /* gm20b_gr_init_fe_pwr_mode_force_on: req_send | mode, poll req done */
    ga10b_wr(GA10B_GR_FE_PWR_MODE, 0x10u | (force_on ? 2u : 0u));
    return ga10b_poll(GA10B_GR_FE_PWR_MODE, 0x10u, 0, 200);
}

static uint32_t gr_ctx_data(uint64_t inst_phy) {
    return (uint32_t)(inst_phy >> 12) |
           GA10B_FECS_CURRENT_CTX_NCOH | GA10B_FECS_CURRENT_CTX_VALID;
}

/* graphics-only PRI ranges skipped by the compute-only golden path
 * (ga10b_gr_init_is_allowed_reg, MIG filter table) */
static int gr_reg_is_compute(uint32_t addr) {
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
        if (addr >= gfx[i][0] && addr <= gfx[i][1])
            return 0;
    return 1;
}

/* sw_ctx_load: aiv list applied as direct PRI writes (obj_ctx.c
 * init_hw_state); index field is unused for the golden image.  nvgpu only
 * applies the is_allowed_reg filter when NVGPU_SUPPORT_MIG is enabled. */
static void gr_load_ctx_load(const void* img, uint32_t size) {
    uint32_t n = ga10b_netlist_count(img, size, GA10B_NETC_SW_CTX_LOAD, 3);
    uint32_t addr, index, val, skipped = 0;
    int mig = (ga10b.skip & GA10B_SKIP_GFX_CTX) != 0u;
    for (uint32_t i = 0; i < n; i++) {
        if (ga10b_netlist_aiv(img, size, GA10B_NETC_SW_CTX_LOAD, i,
                              &addr, &index, &val) != 0)
            break;
        if (mig && !gr_reg_is_compute(addr)) { skipped++; continue; }
        ga10b_wr(addr, val);
    }
    klog("ga10b: sw_ctx_load %u writes (%u gfx skipped)\n", n - skipped,
         skipped);
}

/* gr_ga10b_dump_gr_status_regs subset: which unit keeps GR busy */
static void gr_idle_dump(const char* where) {
    if (!ga10b.hw)
        return;
    klog("ga10b: gr busy after %s: status=%08x status1=%08x eng=%08x "
         "intr=%08x exc=%08x exc1=%08x fecs_intr=%08x host_int=%08x\n", where,
         ga10b_rd(GA10B_GR_STATUS), ga10b_rd(0x00400604u),
         ga10b_rd(0x0040060cu), ga10b_rd(GA10B_GR_INTR), ga10b_rd(0x00400108u),
         ga10b_rd(0x00400118u), ga10b_rd(0x00400144u),
         ga10b_rd(GA10B_FECS_HOST_INT_STATUS));
    klog("ga10b: gr activity0=%08x 1=%08x 4=%08x sked=%08x gpc0 act0=%08x "
         "1=%08x 2=%08x 3=%08x 4=%08x tpc0=%08x go_idle_info=%08x\n",
         ga10b_rd(0x00400380u), ga10b_rd(0x00400384u), ga10b_rd(0x00400390u),
         ga10b_rd(0x00407054u), ga10b_rd(0x00502c80u), ga10b_rd(0x00502c84u),
         ga10b_rd(0x00502c88u), ga10b_rd(0x00502c8cu), ga10b_rd(0x00502c9cu),
         ga10b_rd(0x00504500u), ga10b_rd(0x00404194u));
    klog("ga10b: gr gpc0 exc=%08x tpc0 exc=%08x fecs mbox0=%08x mbox6=%08x "
         "cur_ctx=%08x status1=%08x\n",
         ga10b_rd(0x00502c90u), ga10b_rd(0x00504508u),
         ga10b_rd(GA10B_FECS_CTXSW_MBOX(0)), ga10b_rd(GA10B_FECS_CTXSW_MBOX(6)),
         ga10b_rd(GA10B_FECS_CURRENT_CTX), ga10b_rd(GA10B_FECS_CTXSW_STATUS1));
}

/* ga10b_gr_intr_set_hww_esr_report_mask */
static void gr_hww_masks(void) {
    uint32_t w0 = ga10b_rd(GA10B_GR_SM0_HWW_WARP_MASK);
    uint32_t g0 = ga10b_rd(GA10B_GR_SM0_HWW_GLOBAL_MASK);
    ga10b_wr(GA10B_GR_SMS_HWW_GLOBAL_ESR, ~0u);
    ga10b_wr(GA10B_GR_SMS_HWW_WARP_MASK, w0 | GA10B_GR_HWW_WARP_CONST);
    ga10b_wr(GA10B_GR_SMS_HWW_GLOBAL_MASK, g0 | GA10B_GR_HWW_GLOBAL_CONST);
}

static void gr_fe_go_idle_timeout(int enable) {
    ga10b_wr(GA10B_GR_FE_GO_IDLE_TIMEOUT, enable ? 0x1800u : 0u);
    ga10b_wr(GA10B_GR_FE_COMPUTE_GO_IDLE_TIMEOUT, enable ? 0x40u : 0u);
}

/* ga10b_gr_init_auto_go_idle: graphics (bit17) and compute (bit18) */
static void gr_auto_go_idle(int enable) {
    ga10b_wr_mask(GA10B_GR_DEBUG_2, 3u << 17, enable ? (3u << 17) : 0u);
}

static int gr_bundle_stuck(const char* list, uint32_t i, uint32_t n,
                           uint32_t addr) {
    klog("ga10b: %s[%u/%u] addr=%08x stuck gr_status=%08x\n", list, i, n,
         addr, ga10b_rd(GA10B_GR_STATUS));
    return -1;
}

/* ga10b_gr_init_commit_global_timeslice: RMW direct writes */
static void gr_timeslice(void) {
    ga10b_wr(GA10B_GR_PE_VAF, ga10b_rd(GA10B_GR_PE_VAF) | 0x10u);
    ga10b_wr(GA10B_GR_PES_VSC_VPC, ga10b_rd(GA10B_GR_PES_VSC_VPC) | 0x4u);
    ga10b_wr(GA10B_GR_PD_AB_DIST_CFG0,
             ga10b_rd(GA10B_GR_PD_AB_DIST_CFG0) | 0x80000000u);
}

/* ga10b_gr_init_fs_state */
static void gr_fs_state(void) {
    ga10b_wr_mask(GA10B_GR_SM_TEXIO_CTRL, 0x700u, 0x100u);
    ga10b_wr_mask(GA10B_GR_SM_DISP_CTRL, 0x1800u, 0x1000u);
    ga10b_wr_mask(GA10B_GR_DEBUG_0, 0x800u, 0x800u);
    ga10b_wr_mask(GA10B_GR_MPC_PIX_DEBUG, 0x2000u, 0u);
}

/* fs_state.c: sm_id_numbering + pd/ds tpc-per-gpc + cwd_fs + sm_id_config
 * (gv11b).  Everything follows the sm-id table built by gr_sm_id_table. */
/* ga10b_gr_init_sm_id_numbering for every SM (nvgpu_gr_init_sm_id_early_
 * config): must be in place before the ctxsw falcons start.  Register
 * offsets use the PES-ordered tpc index, values the global tpc index. */
static void gr_sm_id_numbering(void) {
    for (uint32_t sm = 0; sm < gr.sm_total; sm++) {
        uint32_t gtpc = sm / GA10B_SM_PER_TPC;
        uint32_t g = gr.sm_gpc[gtpc];
        uint32_t t = gr_nonpes_tpc(g, gr.sm_tpc[gtpc]);
        ga10b_wr(GA10B_GR_GPC0_SM_CFG + g * GA10B_GPC_STRIDE +
                 t * GA10B_TPC_STRIDE, gtpc);
        ga10b_wr(GA10B_GR_GPC0_GPM_PD_SM_ID(t) + g * GA10B_GPC_STRIDE, gtpc);
    }
}

static void gr_sm_id_programming(void) {
    uint32_t cwd_tpc[4] = { 0, 0, 0, 0 };   /* 4 tpcs per reg, 8 bits each */
    uint32_t cwd_sm[16] = { 0 };

    gr_sm_id_numbering();
    for (uint32_t gtpc = 0; gtpc < gr.tpc_total; gtpc++) {
        uint32_t g = gr.sm_gpc[gtpc];
        uint32_t t = gr.sm_tpc[gtpc];
        cwd_tpc[gtpc / 4u] |= ((g << 4) | t) << ((gtpc % 4u) * 8u);
        uint32_t idx = g + GA10B_MAX_GPC * ((t & 4u) >> 2);
        cwd_sm[idx] |= gtpc << ((t & 3u) * 8u);
    }
    for (uint32_t i = 0; i < (gr.tpc_total + 3u) / 4u; i++)
        ga10b_wr(GA10B_GR_CWD_GPC_TPC_ID(i), cwd_tpc[i]);
    for (uint32_t i = 0; i < 16u; i++)
        ga10b_wr(GA10B_GR_CWD_SM_ID(i), cwd_sm[i]);

    /* pd/ds num_tpc_per_gpc: 4 regs x 8 gpcs x 4-bit counts */
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t v = 0;
        for (uint32_t j = 0; j < 8u; j++) {
            uint32_t g = i * 8u + j;
            uint32_t cnt = g < gr.gpc_count ?
                           popcount32(gr.gpc_tpc_mask[g]) : 0u;
            v |= (cnt & 0xfu) << (j * 4u);
        }
        ga10b_wr(GA10B_GR_PD_NUM_TPC_PER_GPC(i), v);
        ga10b_wr(GA10B_GR_DS_NUM_TPC_PER_GPC(i), v);
    }
    ga10b_wr(GA10B_GR_CWD_FS,
             (gr.gpc_count & 0xffu) | ((gr.tpc_total & 0xffu) << 8));
}

/* gm20b_gr_init_load_sw_bundle_init (the ga10b HAL choice outside
 * CONFIG_NVGPU_GR_GOLDEN_CTX_VERIFICATION): av list through the pipe
 * bundle PRI, values written unmodified */
static int gr_load_bundle(const void* img, uint32_t size) {
    uint32_t n = ga10b_netlist_count(img, size, GA10B_NETC_SW_BUNDLE_INIT, 2);
    uint32_t last = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr, val;
        if (ga10b_netlist_av(img, size, GA10B_NETC_SW_BUNDLE_INIT, i,
                             &addr, &val) != 0)
            return -1;
        if (i == 0 || val != last) {
            ga10b_wr(GA10B_GR_PIPE_BUNDLE_DATA, val);
            last = val;
        }
        ga10b_wr(GA10B_GR_PIPE_BUNDLE_ADDR, addr);
        if ((addr & 0xffffu) == GA10B_GR_BUNDLE_GO_IDLE &&
            gr_wait_idle() != 0)
            return gr_bundle_stuck("sw_bundle_init", i, n, addr);
        if (gr_wait_fe_idle() != 0)
            return gr_bundle_stuck("sw_bundle_init fe", i, n, addr);
    }
    return 0;
}

/* gv11b_gr_init_load_sw_veid_bundle: each non-idle entry is replayed for
 * all 64 subcontexts with the veid field (bits 25:20) */
static int gr_load_veid_bundle(const void* img, uint32_t size) {
    uint32_t n = ga10b_netlist_count(img, size,
                                     GA10B_NETC_SW_VEID_BUNDLE_INIT, 2);
    uint32_t last = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr, val;
        if (ga10b_netlist_av(img, size, GA10B_NETC_SW_VEID_BUNDLE_INIT, i,
                             &addr, &val) != 0)
            return -1;
        if (i == 0 || val != last) {
            ga10b_wr(GA10B_GR_PIPE_BUNDLE_DATA, val);
            last = val;
        }
        if ((addr & 0xffffu) == GA10B_GR_BUNDLE_GO_IDLE) {
            ga10b_wr(GA10B_GR_PIPE_BUNDLE_ADDR, addr);
            if (gr_wait_idle() != 0)
                return gr_bundle_stuck("sw_veid_bundle", i, n, addr);
        } else {
            for (uint32_t j = 0; j < GA10B_GR_MAX_SUBCTX; j++) {
                ga10b_wr(GA10B_GR_PIPE_BUNDLE_ADDR, addr | (j << 20));
                if (gr_wait_fe_idle() != 0)
                    return gr_bundle_stuck("sw_veid_bundle fe", i, n, addr);
            }
        }
    }
    return 0;
}

/* tu104_gr_init_load_sw_bundle64: av64 list, data lo+hi then address */
static int gr_load_bundle64(const void* img, uint32_t size) {
    uint32_t n = ga10b_netlist_count(img, size,
                                     GA10B_NETC_SW_BUNDLE64_INIT, 3);
    uint32_t last_lo = 0, last_hi = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr, lo, hi;
        if (ga10b_netlist_av64(img, size, GA10B_NETC_SW_BUNDLE64_INIT, i,
                               &addr, &lo, &hi) != 0)
            return -1;
        if (i == 0 || lo != last_lo || hi != last_hi) {
            ga10b_wr(GA10B_GR_PIPE_BUNDLE_DATA, lo);
            ga10b_wr(GA10B_GR_PIPE_BUNDLE_DATA_HI, hi);
            last_lo = lo;
            last_hi = hi;
        }
        ga10b_wr(GA10B_GR_PIPE_BUNDLE_ADDR, addr);
        if ((addr & 0xffffu) == GA10B_GR_BUNDLE_GO_IDLE) {
            if (gr_wait_idle() != 0)
                return gr_bundle_stuck("sw_bundle64", i, n, addr);
        } else if (gr_wait_fe_idle() != 0) {
            return gr_bundle_stuck("sw_bundle64 fe", i, n, addr);
        }
    }
    return 0;
}

/* gm20b_gr_init_load_method_init: mme shadow ram data/index */
static void gr_load_method_init(const void* img, uint32_t size) {
    uint32_t n = ga10b_netlist_count(img, size, GA10B_NETC_SW_METHOD_INIT, 2);
    uint32_t last = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr, val;
        if (ga10b_netlist_av(img, size, GA10B_NETC_SW_METHOD_INIT, i,
                             &addr, &val) != 0)
            break;
        if (i == 0 || val != last) {
            ga10b_wr(GA10B_GR_MME_SHADOW_DATA, val);
            last = val;
        }
        ga10b_wr(GA10B_GR_MME_SHADOW_INDEX, 0x80000000u | addr);
    }
    klog("ga10b: sw_method_init %u entries\n", n);
}

/* ---- graphics global ctx buffers + floorsweep maps.  Default (non-MIG)
 * nvgpu commits these before the sw bundles; the bundles travel through the
 * bundle cb, so without it the final GO_IDLE bundle never drains. ---- */
enum { GR_GBUF_PAGEPOOL, GR_GBUF_BUNDLE, GR_GBUF_ATTRIB, GR_GBUF_RTV,
       GR_GBUF_N };
#define GR_BUNDLE_CB_256B      0x30u     /* gr_scc_bundle_cb_size_div_256b__prod */
#define GR_PAGEPOOL_PAGES      0x200u    /* gr_scc_pagepool_total_pages_hwmax_value */
#define GR_RTV_CB_256B         0x800u    /* gr_scc_rm_rtv_cb_size_div_256b_default */
#define GR_CB_DEFAULT          0x800u    /* cbm beta/alpha cb_size_v_default */
#define GR_CB_GRANULARITY      0x20u
#define GR_PPC_STRIDE          0x200u    /* proj_ppc_in_gpc_stride */

typedef struct { uint32_t* buf; uint32_t count; } gr_patch_t;

/* nvgpu_gr_ctx_patch_write: direct PRI write, or a ctx patch-buffer entry */
static void gr_ctx_wr(gr_patch_t* p, uint32_t addr, uint32_t val) {
    if (p == NULL)
        ga10b_wr(addr, val);
    else
        (void)ga10b_patch_add(p->buf, &p->count, addr, val);
}

static uint32_t gr_attrib_cb_size(void) {
    uint32_t lim = 0x3fffffu / gr.tpc_total;
    return lim < GR_CB_DEFAULT ? lim : GR_CB_DEFAULT;
}

static uint32_t gr_alpha_cb_size(void) {
    uint32_t lim = 0xffffu / gr.tpc_total;
    return lim < GR_CB_DEFAULT ? lim : GR_CB_DEFAULT;
}

/* gv11b_gr_init_get_global_attr_cb_size */
static uint32_t gr_global_attr_cb_size(void) {
    uint32_t size = (gr_attrib_cb_size() + gr_alpha_cb_size()) *
                    GR_CB_GRANULARITY * gr.max_tpc;
    return (size + 127u) & ~127u;
}

static int gr_global_ctx_alloc(void) {
    if (gr.gbuf[0].va != NULL)
        return 0;
    gr.gbuf[GR_GBUF_PAGEPOOL].size = GR_PAGEPOOL_PAGES * 0x100u;
    gr.gbuf[GR_GBUF_BUNDLE].size = GR_BUNDLE_CB_256B * 0x100u;
    gr.gbuf[GR_GBUF_ATTRIB].size = gr_global_attr_cb_size();
    gr.gbuf[GR_GBUF_RTV].size = GR_RTV_CB_256B * 0x100u;
    for (uint32_t i = 0; i < GR_GBUF_N; i++)
        if (ga10b_dma_alloc(gr.gbuf[i].size, 4096, &gr.gbuf[i].va,
                            &gr.gbuf[i].phy) != 0)
            return -1;
    return 0;
}

static int gr_global_ctx_map(ga10b_gmmu_t* mm, uint64_t* va) {
    for (uint32_t i = 0; i < GR_GBUF_N; i++) {
        va[i] = ga10b_gmmu_map(mm, gr.gbuf[i].phy, gr.gbuf[i].size);
        if (va[i] == 0)
            return -1;
    }
    return 0;
}

/* nvgpu_gr_obj_ctx_commit_global_ctx_buffers (support_gfx): pagepool,
 * bundle cb, attrib cb, cb manager, rtv cb */
static void gr_commit_global_ctx_buffers(gr_patch_t* p, const uint64_t* va) {
    /* gp10b commit_global_pagepool: the default size is written as hwmax 0 */
    uint32_t a = (uint32_t)(va[GR_GBUF_PAGEPOOL] >> 8);
    gr_ctx_wr(p, 0x0040800cu, a);
    gr_ctx_wr(p, 0x00408010u, 0x80000000u);
    gr_ctx_wr(p, 0x00419004u, a);
    gr_ctx_wr(p, 0x00419008u, 0u);

    /* ga10b commit_global_bundle_cb */
    a = (uint32_t)(va[GR_GBUF_BUNDLE] >> 8);
    gr_ctx_wr(p, 0x00408004u, a);
    gr_ctx_wr(p, 0x00408008u, GR_BUNDLE_CB_256B | 0x80000000u);
    gr_ctx_wr(p, 0x00418e24u, a);
    gr_ctx_wr(p, 0x00418e28u, GR_BUNDLE_CB_256B | 0x80000000u);
    uint32_t state_limit = GR_BUNDLE_CB_256B * 0x100u / 0x20u;
    if (state_limit > 0x480u)
        state_limit = 0x480u;              /* min_gpm_fifo_depths */
    gr_ctx_wr(p, 0x004064c8u, 0x480u | (state_limit << 16));

    /* gv11b commit_global_attrib_cb (gm20b part first) */
    a = (uint32_t)(va[GR_GBUF_ATTRIB] >> 12);
    gr_ctx_wr(p, 0x00418810u, (a & 0x0fffffffu) | 0x80000000u);
    gr_ctx_wr(p, 0x00419848u, (a & 0x0fffffffu) | 0x10000000u);
    gr_ctx_wr(p, 0x00419c2cu, (a & 0x0fffffffu) | 0x10000000u);
    gr_ctx_wr(p, 0x00419c2cu, (a & 0x0fffffffu) | 0x10000000u);
    gr_ctx_wr(p, 0x00419e00u, a);
    gr_ctx_wr(p, 0x00419e04u,
              ((gr_global_attr_cb_size() / 0x80u) & 0x1fffffu) | 0x80000000u);

    /* gp10b commit_global_cb_manager */
    uint32_t attrib_size = gr_attrib_cb_size();
    uint32_t alpha_size = gr_alpha_cb_size();
    gr_ctx_wr(p, 0x00405830u, GR_CB_DEFAULT);
    gr_ctx_wr(p, 0x0040585cu, GR_CB_DEFAULT);
    gr_ctx_wr(p, 0x004064c4u,
              ((GR_CB_DEFAULT * GR_CB_GRANULARITY / 0x80u) << 16) | 0xffffu);
    uint32_t alpha_off = 0;
    uint32_t attrib_off = gr.tpc_total * alpha_size;
    for (uint32_t g = 0; g < gr.gpc_count; g++) {
        for (uint32_t pp = 0; pp < gr.gpc_ppc_count[g]; pp++) {
            uint32_t n = popcount32(gr.pes_tpc_mask[pp][g]);
            uint32_t base = g * GA10B_GPC_STRIDE + pp * GR_PPC_STRIDE;
            gr_ctx_wr(p, 0x005030c0u + base, GR_CB_DEFAULT * n);
            gr_ctx_wr(p, 0x005030f4u + base, attrib_off);
            gr_ctx_wr(p, 0x005030f0u + base, GR_CB_DEFAULT * n);
            attrib_off += attrib_size * n;
            gr_ctx_wr(p, 0x005030e4u + base, GR_CB_DEFAULT * n);
            gr_ctx_wr(p, 0x005030f8u + base, alpha_off);
            alpha_off += alpha_size * n;
            gr_ctx_wr(p, 0x00418ea0u + 4u * (pp + GA10B_PES_PER_GPC * g),
                      (GR_CB_DEFAULT * n) & 0x3fffffu);
        }
    }

    /* tu104 commit_rtv_cb */
    a = (uint32_t)(va[GR_GBUF_RTV] >> 8);
    gr_ctx_wr(p, 0x00408070u, a);
    gr_ctx_wr(p, 0x00408074u, GR_RTV_CB_256B);
    gr_ctx_wr(p, 0x00419034u, a);
    gr_ctx_wr(p, 0x00408078u, 0u);
}

/* gr_config_init_gpc_skip_mask */
static uint32_t gr_gpc_skip_mask(uint32_t g) {
    uint32_t c0 = popcount32(gr.pes_tpc_mask[0][g]);
    uint32_t c1 = popcount32(gr.pes_tpc_mask[1][g]);
    if (c0 + c1 != 5u && !(c0 + c1 == 4u && c0 != c1))
        return 0;
    uint32_t m = gr.pes_tpc_mask[c0 > c1 ? 0 : 1][g];
    return m ^ (m & (m - 1u));
}

/* nvgpu_gr_config_init_map_tiles + gv11b_gr_init_rop_mapping +
 * gm20b_gr_init_pd_skip_table_gpc: the non-MIG part of fs_state_init */
static void gr_rop_mapping(void) {
    static const uint8_t primes[] = { 3, 5, 7, 11, 13, 17, 19, 23 };
    uint32_t tpc = gr.tpc_total;
    uint32_t row = 3;
    if (tpc == 3u) {
        row = 2;
    } else if (tpc < 3u) {
        row = 1;
    } else {
        for (uint32_t i = 0; i < sizeof(primes); i++)
            if (tpc % primes[i] != 0u) { row = primes[i]; break; }
    }
    if (tpc == 7u || tpc == 5u)
        row = 1;

    uint8_t tiles[GR_MAX_TPC + GA10B_MAX_GPC];
    uint32_t sorted[GA10B_MAX_GPC], order[GA10B_MAX_GPC];
    int32_t frac[GA10B_MAX_GPC], err[GA10B_MAX_GPC];
    uint32_t max_cnt = 0;
    memset(tiles, 0, sizeof(tiles));
    for (uint32_t g = 0; g < gr.gpc_count; g++) {
        sorted[g] = gr.gpc_tpc_count[g];
        order[g] = g;
        if (sorted[g] > max_cnt)
            max_cnt = sorted[g];
    }
    for (uint32_t pass = 0; pass + 1u < gr.gpc_count; pass++)
        for (uint32_t g = 0; g + 1u < gr.gpc_count; g++)
            if (sorted[g + 1u] > sorted[g]) {
                uint32_t t = sorted[g]; sorted[g] = sorted[g + 1u];
                sorted[g + 1u] = t;
                t = order[g]; order[g] = order[g + 1u]; order[g + 1u] = t;
            }
    int32_t mul = ((gr.gpc_count * max_cnt) & 1u) ? 2 : 1;
    int32_t denom = (int32_t)(gr.gpc_count * max_cnt) * mul;
    for (uint32_t g = 0; g < gr.gpc_count; g++) {
        frac[g] = (int32_t)(sorted[g] * gr.gpc_count) * mul;
        err[g] = frac[g] + (sorted[g] != 0u ?
                 (int32_t)(g * max_cnt) * mul - denom / 2 : 0);
    }
    for (uint32_t mark = 0; mark < tpc; ) {
        for (uint32_t g = 0; g < gr.gpc_count; g++) {
            if (err[g] * 2 >= denom) {
                tiles[mark++] = (uint8_t)order[g];
                err[g] += frac[g] - denom;
            } else {
                err[g] += frac[g];
            }
        }
    }

    uint32_t cfg = (row & 0xffu) | ((tpc & 0xffu) << 8);
    ga10b_wr(0x00418bb8u, cfg);                    /* crstr_map_table_cfg */
    for (uint32_t r = 0; r < (GR_MAX_TPC + 5u) / 6u; r++) {
        uint32_t map = 0;
        for (uint32_t o = 0; o < 6u && r * 6u + o < GR_MAX_TPC; o++)
            map |= (uint32_t)(tiles[r * 6u + o] & 0x1fu) << (5u * o);
        ga10b_wr(0x00418b08u + 4u * r, map);       /* crstr_gpc_map */
        ga10b_wr(0x0041bf00u + 4u * r, map);       /* ppcs_wwdx_map_gpc_map */
        ga10b_wr(0x0040780cu + 4u * r, map);       /* rstr2d_gpc_map */
    }
    ga10b_wr(0x0041bfd0u, cfg);                    /* wwdx_map_table_cfg */
    for (uint32_t i = 0, j = 1; i < 5u; i++, j += 4u)
        ga10b_wr(0x0041bfb0u + 4u * i,
                 ((1u << j) % tpc) | (((1u << (j + 1u)) % tpc) << 8) |
                 (((1u << (j + 2u)) % tpc) << 16) |
                 (((1u << (j + 3u)) % tpc) << 24));
    ga10b_wr(0x004078bcu, cfg);                    /* rstr2d_map_table_cfg */

    uint32_t skip = 0;
    for (uint32_t i = 0; i < 8u; i++) {
        for (uint32_t k = 0; k < 4u; k++) {
            uint32_t g = i * 4u + k;
            if (g < gr.gpc_count && (gr_gpc_skip_mask(g) & 0xffu) != 0u)
                skip = 1;
        }
        ga10b_wr(0x004064d0u + 4u * i, skip);      /* pd_dist_skip_table */
    }
}

static void gr_gate_list(const ga10b_gate_t* t, uint32_t n);

static int gr_golden_init(void) {
    const void* img = gr.netc.va;
    uint32_t size = gr.netc.size;

    /* dedicated VM + surfaces (obj_ctx prepare_inst_block/prepare_gr_ctx);
     * inst block is RAMIN-only: no RAMFC, no eng_method_buffer */
    if (ga10b_gmmu_init(&gr.golden_mm) != 0)
        return -1;
    if (ga10b_dma_alloc(4096, 4096, &gr.g_inst_va, &gr.g_inst_phy) != 0 ||
        ga10b_dma_alloc(4096, 4096, &gr.g_hdr_va, &gr.g_hdr_phy) != 0 ||
        ga10b_dma_alloc(4096, 4096, &gr.g_patch_va, &gr.g_patch_phy) != 0 ||
        ga10b_dma_alloc(GA10B_GR_ACCESS_MAP_SIZE, 4096,
                        &gr.g_map_va, &gr.g_map_phy) != 0)
        return -1;
    gr.g_ctx_gpu_va = ga10b_gmmu_map(&gr.golden_mm, gr.golden_phy,
                                     gr.ctx_image_size);
    gr.g_hdr_gpu_va = ga10b_gmmu_map(&gr.golden_mm, gr.g_hdr_phy, 4096);
    gr.g_patch_gpu_va = ga10b_gmmu_map(&gr.golden_mm, gr.g_patch_phy, 4096);
    gr.g_map_gpu_va = ga10b_gmmu_map(&gr.golden_mm, gr.g_map_phy,
                                     GA10B_GR_ACCESS_MAP_SIZE);
    if (gr.g_ctx_gpu_va == 0 || gr.g_hdr_gpu_va == 0 ||
        gr.g_patch_gpu_va == 0 || gr.g_map_gpu_va == 0)
        return -1;
    int gfx = (ga10b.skip & GA10B_SKIP_GFX_CTX) == 0u;
    if (gfx && (gr_global_ctx_alloc() != 0 ||
                gr_global_ctx_map(&gr.golden_mm, gr.g_gbuf_va) != 0))
        return -1;

    memset(gr.g_hdr_va, 0, GA10B_CTXSW_HDR_BYTES);
    ga10b_ramin_gr((uint32_t*)gr.g_inst_va, gr.golden_mm.pdb_phy,
                   gr.g_hdr_gpu_va);
    ga10b_ctxsw_veid_hdr((uint32_t*)gr.g_hdr_va, gr.g_ctx_gpu_va,
                         gr.g_patch_gpu_va, gr.g_map_gpu_va);
    __asm__ volatile("dsb sy" ::: "memory");

    /* init_hw_state */
    if (gr_fe_pwr(1) != 0)
        return -1;
    gr_override_context_reset();
    if (gr_fe_pwr(0) != 0)
        return -1;
    if (fecs_method(&FECS_BIND_PTR, gr_ctx_data(gr.g_inst_phy), NULL) != 0)
        return -1;
    if (gr_wait_idle() != 0) {
        gr_idle_dump("bind_ptr");
        return -1;
    }
    gr_load_ctx_load(img, size);
    gr_hww_masks();
    /* nvgpu_cg_blcg_gr_load_enable: sw_ctx_load rewrites the BLCG registers */
    if (!(ga10b.skip & GA10B_SKIP_GR_GATING))
        gr_gate_list(ga10b_blcg_gr, GA10B_GATE_N(ga10b_blcg_gr));
    if (gr_wait_idle() != 0) {
        gr_idle_dump("sw_ctx_load");
        return -1;
    }

    /* commit_hw_state */
    gr_fe_go_idle_timeout(0);
    if (gfx)
        gr_commit_global_ctx_buffers(NULL, gr.g_gbuf_va);
    gr_timeslice();
    gr_fs_state();
    gr_sm_id_programming();
    if (gfx)
        gr_rop_mapping();
    if (gr_wait_idle() != 0) {
        gr_idle_dump("fs_state");
        return -1;
    }
    gr_auto_go_idle(0);
    ga10b_wr(GA10B_GR_PIPE_BUNDLE_CFG, 0x80000000u);  /* pipe override on */
    if (gr_load_bundle(img, size) != 0 ||
        gr_load_veid_bundle(img, size) != 0 ||
        gr_load_bundle64(img, size) != 0) {
        gr_idle_dump("sw_bundle");
        ga10b_wr(GA10B_GR_PIPE_BUNDLE_CFG, 0);
        return -1;
    }
    ga10b_wr(GA10B_GR_PIPE_BUNDLE_CFG, 0);
    if (gr_wait_idle() != 0) {
        gr_idle_dump("pipe_override_off");
        return -1;
    }
    gr_auto_go_idle(1);
    gr_fe_go_idle_timeout(1);
    gr_load_method_init(img, size);
    if (gr_wait_idle() != 0) {
        gr_idle_dump("method_init");
        return -1;
    }

    /* nvgpu_gr_ctx_init_zcull: zcull mode no_ctxsw, no zcull buffer */
    if (gfx) {
        uint32_t* c = (uint32_t*)gr.golden_va;
        c[0x1cu / 4u] = 1u;
        c[0x20u / 4u] = 0u;
        c[0x60u / 4u] = 0u;
        __asm__ volatile("dsb sy" ::: "memory");
    }

    /* save_golden_ctx: FECS method 0x9, then CPU copy to local image */
    if (fecs_method(&FECS_GOLDEN_SAVE, gr_ctx_data(gr.g_inst_phy),
                    NULL) != 0)
        return -1;
    gr.golden_local = malloc(gr.ctx_image_size);
    if (gr.golden_local == NULL)
        return -1;
    memcpy(gr.golden_local, gr.golden_va, gr.ctx_image_size);
    ga10b_wr(GA10B_FECS_CURRENT_CTX, 0);   /* set_current_ctx_invalid */
    klog("ga10b: golden ctx saved (%u bytes)\n",
         gr.ctx_image_size);
    return 0;
}

int ga10b_gr_ctx_prepare(ga10b_chan_t* ch) {
    if (!gr.ready || gr.golden_local == NULL) {
        klog("ga10b: gr_ctx_prepare before golden init\n");
        return -1;
    }
    if (ch->gr_ctx_va != NULL)
        return 0;
    /* RAMIN GR fields must land before PBDMA first fetches the instance
     * block, i.e. before any work is pushed on this channel */
    if (ga10b.hw && ch->gpfifo_put != 0) {
        klog("ga10b: gr_ctx_prepare after channel submit\n");
        return -1;
    }
    if (ga10b_dma_alloc(gr.ctx_image_size, 4096,
                        &ch->gr_ctx_va, &ch->gr_ctx_phy) != 0 ||
        ga10b_dma_alloc(4096, 4096, &ch->gr_patch_va,
                        &ch->gr_patch_phy) != 0 ||
        ga10b_dma_alloc(4096, 4096, &ch->gr_hdr_va, &ch->gr_hdr_phy) != 0)
        goto fail;
    ch->gr_ctx_gpu_va = ga10b_gmmu_map(ch->mm, ch->gr_ctx_phy,
                                       gr.ctx_image_size);
    ch->gr_patch_gpu_va = ga10b_gmmu_map(ch->mm, ch->gr_patch_phy, 4096);
    ch->gr_hdr_gpu_va = ga10b_gmmu_map(ch->mm, ch->gr_hdr_phy, 4096);
    /* shared zeroed priv access map (deny-all; compute needs no priv) */
    uint64_t map_va = ga10b_gmmu_map(ch->mm, gr.g_map_phy,
                                     GA10B_GR_ACCESS_MAP_SIZE);
    if (ch->gr_ctx_gpu_va == 0 || ch->gr_patch_gpu_va == 0 ||
        ch->gr_hdr_gpu_va == 0 || map_va == 0)
        goto fail;

    /* global ctx buffers live at this channel's own VAs: patch them over
     * the golden-VM addresses captured in the golden image */
    gr_patch_t patch = { (uint32_t*)ch->gr_patch_va, 0 };
    if (gr.gbuf[0].va != NULL) {
        uint64_t gva[GR_GBUF_N];
        if (gr_global_ctx_map(ch->mm, gva) != 0)
            goto fail;
        gr_commit_global_ctx_buffers(&patch, gva);
    }

    memcpy(ch->gr_ctx_va, gr.golden_local, gr.ctx_image_size);
    ga10b_ctxsw_main_patch((uint32_t*)ch->gr_ctx_va, ch->gr_patch_gpu_va,
                           map_va, patch.count);
    memset(ch->gr_hdr_va, 0, GA10B_CTXSW_HDR_BYTES);
    ga10b_ctxsw_veid_hdr((uint32_t*)ch->gr_hdr_va, ch->gr_ctx_gpu_va,
                         ch->gr_patch_gpu_va, map_va);
    ga10b_ramin_gr((uint32_t*)ch->inst_va, ch->mm->pdb_phy,
                   ch->gr_hdr_gpu_va);
    __asm__ volatile("dsb sy" ::: "memory");
    klog("ga10b: chan%u gr ctx loaded (ctx=%llx hdr=%llx)\n", ch->chid,
         (unsigned long long)ch->gr_ctx_gpu_va,
         (unsigned long long)ch->gr_hdr_gpu_va);
    return 0;
fail:
    ga10b_dma_free(ch->gr_ctx_va);
    ga10b_dma_free(ch->gr_patch_va);
    ga10b_dma_free(ch->gr_hdr_va);
    ch->gr_ctx_va = ch->gr_patch_va = ch->gr_hdr_va = NULL;
    return -1;
}

/* plain av list applied as direct PRI writes */
static void gr_load_av_list(uint32_t region) {
    uint32_t n = ga10b_netlist_count(gr.netc.va, gr.netc.size, region, 2);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr, val;
        if (ga10b_netlist_av(gr.netc.va, gr.netc.size, region, i,
                             &addr, &val) != 0)
            break;
        ga10b_wr(addr, val);
    }
    klog("ga10b: netlist region %u: %u writes\n", region, n);
}

/* gm20b_gr_falcon_wait_mem_scrubbing: 1 ms max, 10 us steps */
static int gr_wait_mem_scrubbing(void) {
    if (!ga10b.hw)
        return 0;
    for (uint32_t i = 0; i < 100u; i++) {
        uint32_t f = ga10b_rd(GA10B_FECS_DMACTL);
        uint32_t g = ga10b_rd(GA10B_FECS_DMACTL + GA10B_FECS_GPCCS_OFFSET);
        if (!((f | g) & GA10B_FALCON_DMACTL_SCRUBBING))
            return 0;
        ga10b_delay_us(10);
    }
    klog("ga10b: ctxsw falcon mem scrubbing timeout\n");
    return -1;
}

/* ga10b_gr_init_gpc_mmu: mirror the FB MMU configuration into the GPCs */
static void gr_gpc_mmu(void) {
    ga10b_wr(GA10B_GR_GPCS_MMU_CTRL,
             ga10b_rd(GA10B_FB_MMU_CTRL) & GA10B_GR_GPCS_MMU_CTRL_MASK);
    ga10b_wr(GA10B_GR_GPCS_MMU_PM_UNIT_MASK, 0);
    ga10b_wr(GA10B_GR_GPCS_MMU_PM_REQ_MASK, 0);
    ga10b_wr(GA10B_GR_GPCS_MMU_DEBUG_CTRL, ga10b_rd(GA10B_FB_MMU_DEBUG_CTRL));
    ga10b_wr(GA10B_GR_GPCS_MMU_DEBUG_WR, ga10b_rd(GA10B_FB_MMU_DEBUG_WR));
    ga10b_wr(GA10B_GR_GPCS_MMU_DEBUG_RD, ga10b_rd(GA10B_FB_MMU_DEBUG_RD));
    ga10b_wr(GA10B_GR_GPCS_MMU_NUM_ACTIVE_LTCS,
             ga10b_rd(GA10B_FB_MMU_NUM_ACTIVE_LTCS));
}

/* falcon post-mortem: tracepc shows where it halted, exterrstat/addr capture
 * a failed bus/DMA access (e.g. bootloader fetching main ucode from WPR),
 * IMEM head words prove whether ACR actually loaded the ucode. */
static void gr_falcon_dump(const char* name, uint32_t base) {
    if (!ga10b.hw)
        return;
    klog("ga10b: %s cpuctl=%08x bootvec=%08x hwcfg=%08x hwcfg2=%08x "
         "dmactl=%08x engctl=%08x curctx=%08x nxtctx=%08x\n", name,
         ga10b_rd(base + GA10B_FALCON_CPUCTL),
         ga10b_rd(base + GA10B_FALCON_BOOTVEC),
         ga10b_rd(base + GA10B_FALCON_HWCFG),
         ga10b_rd(base + GA10B_FALCON_HWCFG2),
         ga10b_rd(base + GA10B_FALCON_DMACTL),
         ga10b_rd(base + GA10B_FALCON_ENGCTL),
         ga10b_rd(base + GA10B_FALCON_CURCTX),
         ga10b_rd(base + GA10B_FALCON_NXTCTX));
    klog("ga10b: %s os=%08x dbg1=%08x dbginfo=%08x imstat=%08x "
         "traceidx=%08x tracepc=%08x irqstat=%08x\n", name,
         ga10b_rd(base + GA10B_FALCON_OS),
         ga10b_rd(base + GA10B_FALCON_DEBUG1),
         ga10b_rd(base + GA10B_FALCON_DEBUGINFO),
         ga10b_rd(base + GA10B_FALCON_IMSTAT),
         ga10b_rd(base + GA10B_FALCON_TRACEIDX),
         ga10b_rd(base + GA10B_FALCON_TRACEPC),
         ga10b_rd(base + GA10B_FALCON_IRQSTAT));
    klog("ga10b: %s exterrstat=%08x exterraddr=%08x\n", name,
         ga10b_rd(base + GA10B_FALCON_EXTERRSTAT),
         ga10b_rd(base + GA10B_FALCON_EXTERRADDR));
    /* manually address IMEM words (auto-increment read unreliable): head
     * words + a sample deep in the app-code region prove ucode presence */
    uint32_t iw[4];
    for (uint32_t i = 0; i < 4u; i++) {
        ga10b_wr(base + GA10B_FALCON_IMEMC(0), GA10B_FALCON_IMEMC_OFFS(i));
        iw[i] = ga10b_rd(base + GA10B_FALCON_IMEMD(0));
    }
    ga10b_wr(base + GA10B_FALCON_IMEMC(0), GA10B_FALCON_IMEMC_OFFS(0x100u));
    uint32_t deep = ga10b_rd(base + GA10B_FALCON_IMEMD(0));
    klog("ga10b: %s imem[0..3]=%08x %08x %08x %08x imem[0x400]=%08x\n",
         name, iw[0], iw[1], iw[2], iw[3], deep);
    /* peek the IMEM words around the halt PC (tracepc) to identify the
     * halting instruction / wait loop */
    uint32_t pc = ga10b_rd(base + GA10B_FALCON_TRACEPC) & 0xffffffu;
    uint32_t wa = pc & ~3u;
    uint32_t blk = wa >> 8;
    uint32_t w0 = (wa >> 2) & 0x3fu;
    uint32_t pw[4];
    for (uint32_t i = 0; i < 4u; i++) {
        ga10b_wr(base + GA10B_FALCON_IMEMC(0),
                 GA10B_FALCON_IMEMC_BLK(blk) | GA10B_FALCON_IMEMC_OFFS(w0 + i));
        pw[i] = ga10b_rd(base + GA10B_FALCON_IMEMD(0));
    }
    klog("ga10b: %s imem@pc(%x)=%08x %08x %08x %08x\n",
         name, wa, pw[0], pw[1], pw[2], pw[3]);
    uint32_t dw[16];
    for (uint32_t i = 0; i < 16u; i++) {
        ga10b_wr(base + GA10B_FALCON_DMEMC(0), GA10B_FALCON_IMEMC_OFFS(i));
        dw[i] = ga10b_rd(base + GA10B_FALCON_DMEMD(0));
    }
    klog("ga10b: %s dmem[0..3]=%08x %08x %08x %08x\n",
         name, dw[0], dw[1], dw[2], dw[3]);
    klog("ga10b: %s dmem[4..7]=%08x %08x %08x %08x\n",
         name, dw[4], dw[5], dw[6], dw[7]);
    klog("ga10b: %s dmem[8..11]=%08x %08x %08x %08x\n",
         name, dw[8], dw[9], dw[10], dw[11]);
    klog("ga10b: %s dmem[12..15]=%08x %08x %08x %08x\n",
         name, dw[12], dw[13], dw[14], dw[15]);
}

/* whole IMEM or DMEM as hex words, 32 bytes per line, manually addressed */
static void gr_mem_dump(const char* name, uint32_t base, int imem,
                        uint32_t bytes) {
    uint32_t ctl = base + (imem ? GA10B_FALCON_IMEMC(0) : GA10B_FALCON_DMEMC(0));
    uint32_t dat = base + (imem ? GA10B_FALCON_IMEMD(0) : GA10B_FALCON_DMEMD(0));
    const char* kind = imem ? "imem" : "dmem";
    uint32_t sum = 0;
    for (uint32_t a = 0; a < bytes; a += 32u) {
        uint32_t w[8];
        for (uint32_t i = 0; i < 8u; i++) {
            uint32_t wa = a + 4u * i;
            ga10b_wr(ctl, GA10B_FALCON_IMEMC_BLK(wa >> 8) |
                          GA10B_FALCON_IMEMC_OFFS(wa >> 2));
            w[i] = ga10b_rd(dat);
            sum += w[i];
        }
        klog("ga10b: %s %s %05x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
             name, kind, a, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
    klog("ga10b: %s %s end bytes=%u sum=%08x\n", name, kind, bytes, sum);
}

void ga10b_gr_falcon_memdump(void) {
    static const struct { const char* name; uint32_t base; } f[2] = {
        { "fecs", GA10B_FECS_FALCON_BASE },
        { "gpccs", GA10B_GPCCS_FALCON_BASE },
    };
    if (!ga10b.hw) {
        klog("ga10b: falcon memdump needs --hw\n");
        return;
    }
    for (uint32_t i = 0; i < 2u; i++) {
        uint32_t hwcfg = ga10b_rd(f[i].base + GA10B_FALCON_HWCFG);
        gr_falcon_dump(f[i].name, f[i].base);
        gr_mem_dump(f[i].name, f[i].base, 1, (hwcfg & 0x1ffu) * 256u);
        gr_mem_dump(f[i].name, f[i].base, 0, ((hwcfg >> 9) & 0x1ffu) * 256u);
    }
}

static void gr_gate_list(const ga10b_gate_t* t, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        ga10b_wr(t[i].addr, t[i].prod);
}

/* nvgpu_cg_init_gr_load_gating_prod (SLCG then BLCG prod values) followed
 * by nvgpu_cg_elcg_disable_no_wait */
static void gr_load_gating(void) {
    if (!(ga10b.skip & GA10B_SKIP_GR_GATING)) {
        gr_gate_list(ga10b_slcg_bus, GA10B_GATE_N(ga10b_slcg_bus));
        gr_gate_list(ga10b_slcg_chiplet, GA10B_GATE_N(ga10b_slcg_chiplet));
        gr_gate_list(ga10b_slcg_gr, GA10B_GATE_N(ga10b_slcg_gr));
        gr_gate_list(ga10b_slcg_perf, GA10B_GATE_N(ga10b_slcg_perf));
        gr_gate_list(ga10b_slcg_xbar, GA10B_GATE_N(ga10b_slcg_xbar));
        gr_gate_list(ga10b_slcg_hshub, GA10B_GATE_N(ga10b_slcg_hshub));
        gr_gate_list(ga10b_blcg_bus, GA10B_GATE_N(ga10b_blcg_bus));
        gr_gate_list(ga10b_blcg_gr, GA10B_GATE_N(ga10b_blcg_gr));
        gr_gate_list(ga10b_blcg_xbar, GA10B_GATE_N(ga10b_blcg_xbar));
        gr_gate_list(ga10b_blcg_hshub, GA10B_GATE_N(ga10b_blcg_hshub));
    }
    /* ELCG: every engine clock forced to RUN while FECS/GPCCS init */
    if (ga10b.hw) {
        klog("ga10b: therm_gate_ctrl[0]=%08x [1]=%08x (force run)\n",
             ga10b_rd(GA10B_THERM_GATE_CTRL(0)),
             ga10b_rd(GA10B_THERM_GATE_CTRL(1)));
        for (uint32_t i = 0; i < 16u; i++) {
            uint32_t gc = ga10b_rd(GA10B_THERM_GATE_CTRL(i));
            gc = (gc & ~0x3u) | 0x10u;   /* eng_clk=run, idle_holdoff=on */
            ga10b_wr(GA10B_THERM_GATE_CTRL(i), gc);
        }
    }
}

/* ga10b_gr_intr_enable_interrupts: route notify to the CPU, not the GSP (no
 * GSP-RM runs here, so a GSP-routed FECS ctxsw interrupt is never
 * serviced), clear pending, enable the prod mask */
static void gr_intr_enable(void) {
    uint32_t nc = ga10b_rd(GA10B_GR_INTR_NOTIFY_CTRL);
    nc |= GA10B_GR_INTR_NOTIFY_CPU;
    nc &= ~GA10B_GR_INTR_NOTIFY_GSP;
    ga10b_wr(GA10B_GR_INTR_NOTIFY_CTRL, nc);
    ga10b_wr(GA10B_GR_INTR_EN, 0);
    ga10b_wr(GA10B_GR_INTR, 0xffffffffu);
    ga10b_wr(GA10B_GR_INTR_EN, GA10B_GR_INTR_EN_MASK);
}

/* nvgpu_gr_enable_hw after the engine reset: gating prods + ELCG run, then
 * gr_init_prepare_hw_impl.  Not transcribed: PERFMON/BLG unit reset. */
static int gr_prepare_hw(const char* fw_dir) {
    if (gr.netc.va == NULL &&
        ga10b_fw_load(fw_dir, GA10B_FW_NET_C, &gr.netc) != 0)
        return -1;
    gr_load_gating();
    gr_intr_enable();
    ga10b_wr_mask(GA10B_GR_GPFIFO_CTL, GA10B_GR_GPFIFO_CTL_ENABLE,
                  GA10B_GR_GPFIFO_CTL_ENABLE);
    /* non-MIG nvgpu loads the graphics lists too: FECS/GPCCS init checks
     * state those lists program */
    int gfx = !(ga10b.skip & GA10B_SKIP_GFX_NONCTX);
    gr_load_av_list(GA10B_NETC_SW_NON_CTX_LOAD);
    gr_load_av_list(GA10B_NETC_NON_CTX_LOCAL_COMPUTE);
    if (gfx)
        gr_load_av_list(GA10B_NETC_NON_CTX_LOCAL_GFX);
    gr_load_av_list(GA10B_NETC_NON_CTX_GLOBAL_COMPUTE);
    if (gfx)
        gr_load_av_list(GA10B_NETC_NON_CTX_GLOBAL_GFX);
    if (gr_wait_mem_scrubbing() != 0)
        return -1;
    if (gr_wait_idle() != 0) {
        klog("ga10b: gr not idle after enable_hw\n");
        return -1;
    }
    gr.hw_enabled = 1;
    return 0;
}

int ga10b_gr_enable_hw(const char* fw_dir) {
    if (gr.hw_enabled)
        return 0;
    /* nvgpu_init_gr_manager: SMC arbiter timestamp prod (tick enabled) */
    if (!(ga10b.skip & GA10B_SKIP_SMC_TS))
        ga10b_wr_mask(GA10B_SMCARB_TIMESTAMP_CTRL, 1u, 0u);
    /* gr_reset_engine: disable -> GPC reset -> enable */
    ga10b_devtable_t t;
    const ga10b_devinfo_t* d = NULL;
    if (ga10b_device_table(&t) != 0)
        return -1;
    d = ga10b_device_find(&t, GA10B_DEVTYPE_GRAPHICS, 0);
    if (d == NULL && ga10b.hw) {
        klog("ga10b: no GRAPHICS engine in the TOP device table\n");
        return -1;
    }
    if (d != NULL && ga10b_engine_enable(d->reset_id, 0) != 0)
        return -1;
    if (gr_reset_gpcs() != 0)
        return -1;
    if (d != NULL && ga10b_engine_enable(d->reset_id, 1) != 0)
        return -1;
    return gr_prepare_hw(fw_dir);
}

/* post-mortem of a failed ctxsw start.  Only runs after the handshake
 * timed out: nothing pokes the falcons between start and INIT_DONE. */
static void gr_start_postmortem(void) {
    if (ga10b.hw)
        klog("ga10b: fecs start: cpuctl=%08x gpccs=%08x status1=%08x "
             "idle=%08x dmactl=%08x cur_ctx=%08x mbox0=%08x mbox1=%08x "
             "mbox6=%08x\n",
             ga10b_rd(GA10B_FECS_CPUCTL),
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + GA10B_FALCON_CPUCTL),
             ga10b_rd(GA10B_FECS_CTXSW_STATUS1),
             ga10b_rd(GA10B_FECS_CTXSW_IDLESTATE),
             ga10b_rd(GA10B_FECS_DMACTL),
             ga10b_rd(GA10B_FECS_CURRENT_CTX),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(0)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(1)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(6)));
    gr_falcon_dump("gpccs", GA10B_GPCCS_FALCON_BASE);
    gr_falcon_dump("fecs", GA10B_FECS_FALCON_BASE);
    if (ga10b.hw) {
        klog("ga10b: gpccs ctxsw mbox[0..5]=%08x %08x %08x %08x %08x %08x\n",
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + 0x800u),
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + 0x804u),
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + 0x808u),
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + 0x80cu),
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + 0x810u),
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + 0x814u));
        klog("ga10b: fecs ctxsw mbox[0..5]=%08x %08x %08x %08x %08x %08x\n",
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(0)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(1)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(2)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(3)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(4)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(5)));
        klog("ga10b: fecs host_int_status=%08x status_fe0=%08x status1=%08x\n",
             ga10b_rd(GA10B_FECS_HOST_INT_STATUS),
             ga10b_rd(GA10B_FECS_CTXSW_STATUS_FE0),
             ga10b_rd(GA10B_FECS_CTXSW_STATUS1));
        klog("ga10b: gpccs status1=%08x status_gpc0=%08x idle=%08x\n",
             ga10b_rd(GA10B_GPCCS_CTXSW_STATUS1),
             ga10b_rd(GA10B_GPCCS_CTXSW_STATUS_GPC0),
             ga10b_rd(GA10B_GPCCS_CTXSW_IDLESTATE));
        klog("ga10b: gr_intr_status=%08x notify_ctrl=%08x intr_en=%08x\n",
             ga10b_rd(GA10B_GR_INTR),
             ga10b_rd(GA10B_GR_INTR_NOTIFY_CTRL),
             ga10b_rd(GA10B_GR_INTR_EN));
        klog("ga10b: fecs mbox[6..11]=%08x %08x %08x %08x %08x %08x\n",
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(6)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(7)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(8)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(9)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(10)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(11)));
        klog("ga10b: gpc0 gpccs mbox[0..5]=%08x %08x %08x %08x %08x %08x\n",
             ga10b_rd(0x00502800u), ga10b_rd(0x00502804u),
             ga10b_rd(0x00502808u), ga10b_rd(0x0050280cu),
             ga10b_rd(0x00502810u), ga10b_rd(0x00502814u));
        klog("ga10b: gpccs mbox6=%08x mbox7=%08x gpc0_mbox6=%08x "
             "fecs mbox12=%08x mbox13=%08x\n",
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + 0x818u),
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + 0x81cu),
             ga10b_rd(0x00502818u),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(12)),
             ga10b_rd(GA10B_FECS_CTXSW_MBOX(13)));
        klog("ga10b: gpc0_reset_ctl=%08x gpcs_reset_ctl=%08x "
             "gr_status=%08x mc_enable_gr=%08x\n",
             ga10b_rd(GA10B_GPC0_GPCCS_ENGINE_RESET_CTL),
             ga10b_rd(GA10B_GPCS_GPCCS_ENGINE_RESET_CTL),
             ga10b_rd(GA10B_GR_STATUS),
             ga10b_rd(0x00000200u));
    }
}

/* gr_init_setup_hw subset: eng_config + GPC MMU mirror + FECS host
 * interrupt enable.  First power-on runs it AFTER the ctxsw falcons are up
 * (gr_init_support_impl); only the nvgpu_gr_reset path runs it before. */
static int gr_setup_hw(void) {
    ga10b_wr(GA10B_GR_ENGINE_CONFIG,
             (ga10b.skip & GA10B_SKIP_ENG_GFX) ?
                 GA10B_GR_ENGINE_CONFIG_COMPUTE : GA10B_GR_ENGINE_CONFIG_ALL);
    gr_gpc_mmu();
    ga10b_wr(GA10B_FECS_HOST_INT_ENABLE, GA10B_FECS_HOST_INT_ALL);
    if (gr_wait_idle() != 0) {
        klog("ga10b: gr not idle after setup_hw\n");
        return -1;
    }
    return 0;
}

int ga10b_gr_init(const char* fw_dir) {
    if (gr.ready)
        return 0;
    if (!gr.hw_enabled) {
        klog("ga10b: gr_init before gr_enable_hw\n");
        return -1;
    }
    (void)fw_dir;

    /* gr_init_support_impl: config -> live PES mask -> sm-id numbering,
     * all before the ctxsw ucode starts ("the actual sequence expected by
     * ctxsw ucode") */
    gr_topology();
    if (!(ga10b.skip & GA10B_SKIP_PES_MASK))
        gr_live_pes_mask();
    /* nvgpu_gr_fs_state_init runs in setup_hw BEFORE the ctxsw falcons
     * start: sm-id numbering + CWD_GPC_TPC_ID/SM_ID/FS + pd/ds tpc-per-gpc.
     * Program the FE fs topology here (nvgpu timing), not only in the
     * post-INIT_DONE golden-ctx phase. */
    gr_sm_id_programming();
    if (ga10b.hw)
        klog("ga10b: smcarb gpc_map0=%08x sys_pipe_info=%08x ts_ctrl=%08x "
             "live_pes=%08x swdx_pes=%08x\n",
             ga10b_rd(GA10B_SMCARB_PARTITION_GPC_MAP(0)),
             ga10b_rd(GA10B_SMCARB_SYS_PIPE_INFO),
             ga10b_rd(GA10B_SMCARB_TIMESTAMP_CTRL),
             ga10b_rd(GA10B_GR_GPC0_LIVE_PHYS_PES),
             ga10b_rd(GA10B_GR_GPC0_SWDX_PES_MASK));

    /* FECS/GPCCS were authenticated into WPR by the ACR and bootstrapped by
     * the PMU RTOS (ACR-managed LS-PMU, lazy_bootstrap=1; stage_pmu ran the
     * INIT_WPR_REGION + BOOTSTRAP_GR_FALCONS RPCs that write the HS sigs) */
    if (ga10b_acr_bootstrap_ls(GA10B_FALCON_FECS) != 0 ||
        ga10b_acr_bootstrap_ls(GA10B_FALCON_GPCCS) != 0)
        return -1;

    if ((ga10b.skip & GA10B_SKIP_LATE_SETUP) && gr_setup_hw() != 0)
        return -1;

    /* start_gpccs, then start_fecs (gr_falcon_gm20b_fusa.c:83-110): the
     * ctxsw mailboxes are primed before FECS is released */
    if (ga10b.hw)
        klog("ga10b: gpccs pre-start: cpuctl=%08x mb0=%08x mb1=%08x\n",
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + GA10B_FALCON_CPUCTL),
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + GA10B_FALCON_MAILBOX0),
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + GA10B_FALCON_MAILBOX1));
    ga10b_wr(GA10B_FECS_CPUCTL_ALIAS + GA10B_FECS_GPCCS_OFFSET, 0x2);
    fecs_mbox_clear(0, ~0u);
    ga10b_wr(GA10B_FECS_CTXSW_MBOX(1), 1);
    fecs_mbox_clear(6, ~0u);
    ga10b_wr(GA10B_FECS_CPUCTL_ALIAS, 0x2);

    /* wait_ctxsw_ready: handshake INIT_COMPLETE, invalidate the current
     * ctx (secure GPCCS), then program the FECS watchdog */
    if (ga10b_poll(GA10B_FECS_CTXSW_MBOX(0), 0xffffffffu,
                   GA10B_FECS_HANDSHAKE_INIT_DONE, 30000) != 0) {
        klog("ga10b: fecs ctxsw handshake timeout\n");
        gr_start_postmortem();
        return -1;
    }
    klog("ga10b: fecs ctxsw INIT_DONE\n");
    ga10b_wr(GA10B_FECS_CURRENT_CTX, 0);
    if (fecs_method(&FECS_SET_WATCHDOG, GA10B_FECS_WDT_DEFAULT, NULL) != 0)
        return -1;

    /* init_ctx_state: golden image size is returned in mailbox(0) */
    if (fecs_method(&FECS_DISCOVER_IMAGE_SIZE, 0, &gr.ctx_image_size) != 0)
        return -1;
    if (gr.ctx_image_size == 0)
        gr.ctx_image_size = GR_CTX_IMAGE_FALLBACK;   /* dry-run */
    if (ga10b_dma_alloc(gr.ctx_image_size, 4096,
                        &gr.golden_va, &gr.golden_phy) != 0)
        return -1;
    klog("ga10b: gr ctx image %u bytes @ phy %llx\n", gr.ctx_image_size,
         (unsigned long long)gr.golden_phy);

    if (!(ga10b.skip & GA10B_SKIP_LATE_SETUP) && gr_setup_hw() != 0)
        return -1;

    /* golden ctx from the NETC netlist: sw_ctx_load PRI writes +
     * bundle/method init + FECS GOLDEN_SAVE, compute-only.  NETC is the
     * fixed FINAL netlist on ga10b (netlist_ga10b.h: no MAJORV matching).
     * Dry-run runs the full sequence through the logging MMIO wrappers. */
    if (gr_golden_init() != 0) {
        klog("ga10b: golden ctx init failed\n");
        return -1;
    }
    gr.ready = 1;
    return 0;
}

/* allocate + map the QMD and shader-local buffers into the channel VM */
static int gr_dispatch_buffers(ga10b_chan_t* ch) {
    if (gr.buf_mm == ch->mm && gr.qmd_gpu_va != 0)
        return 0;
    if (gr.buf_mm != NULL) {
        klog("ga10b: gr dispatch buffers already bound to another mm\n");
        return -1;
    }
    if (ga10b_dma_alloc(4096, 4096, &gr.qmd_va, &gr.qmd_phy) != 0 ||
        ga10b_dma_alloc(GR_LOCAL_MEM_SIZE, 4096,
                        &gr.local_va, &gr.local_phy) != 0)
        return -1;
    gr.qmd_gpu_va = ga10b_gmmu_map(ch->mm, gr.qmd_phy, 4096);
    gr.local_gpu_va = ga10b_gmmu_map(ch->mm, gr.local_phy, GR_LOCAL_MEM_SIZE);
    if (gr.qmd_gpu_va == 0 || gr.local_gpu_va == 0)
        return -1;
    gr.buf_mm = ch->mm;
    return 0;
}

/* SM hardware-warning state after a launch that never released its
 * semaphore: warp/global ESR of every live TPC plus the GR exception tree */
void ga10b_gr_launch_postmortem(void) {
    if (!ga10b.hw)
        return;
    gr_idle_dump("launch");
    klog("ga10b: hww fe=%08x info=%08x mme=%08x/%08x memfmt=%08x pd=%08x "
         "scc=%08x ds=%08x sked=%08x ssync=%08x\n",
         ga10b_rd(0x00404000u), ga10b_rd(0x004041b0u), ga10b_rd(0x00404490u),
         ga10b_rd(0x00404494u), ga10b_rd(0x00404600u), ga10b_rd(0x00406018u),
         ga10b_rd(0x00408030u), ga10b_rd(0x00405840u), ga10b_rd(0x00407020u),
         ga10b_rd(0x00405a14u));
    klog("ga10b: trapped addr=%08x data=%08x.%08x mme=%08x class_error=%08x\n",
         ga10b_rd(0x00400704u), ga10b_rd(0x0040070cu), ga10b_rd(0x00400708u),
         ga10b_rd(0x00400710u), ga10b_rd(0x00400110u));
    /* warp_esr[15:0] is an error code, not a bitmask (nvgpu hw_gr_ga10b.h):
     * 0x9 illegal encoding, 0xd out-of-range register, 0xe out-of-range
     * address, 0x10 invalid address space, 0x17 MMU fault ... */
    for (uint32_t t = 0; t < gr.tpc_total && t < GA10B_MAX_TPC_PER_GPC; t++)
        klog("ga10b: tpc%u sm0 warp_esr=%08x pc=%08x global_esr=%08x tpc_exc=%08x\n",
             t, ga10b_rd(0x00504730u + t * GA10B_TPC_STRIDE),
             ga10b_rd(0x00504738u + t * GA10B_TPC_STRIDE),
             ga10b_rd(0x00504734u + t * GA10B_TPC_STRIDE),
             ga10b_rd(0x00504508u + t * GA10B_TPC_STRIDE));
}

/* submit one 64-word QMD on the compute channel and wait for its release0
 * semaphore.  The QMD must release ch->sem_gpu_va with ch->sem_val + 1. */
int ga10b_compute_launch(ga10b_chan_t* ch, const uint32_t* qmd_words) {
    if (!gr.ready) {
        klog("ga10b: compute_launch before gr_init\n");
        return -1;
    }
    if (ga10b_gr_ctx_prepare(ch) != 0)
        return -1;
    if (gr_dispatch_buffers(ch) != 0)
        return -1;
    uint64_t qmd_gpu_va = gr.qmd_gpu_va;
    uint32_t payload = ++ch->sem_val;
    memcpy(gr.qmd_va, qmd_words, 64 * 4);
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t* pb = ga10b_pb_reserve(ch, GR_DISPATCH_WORDS);
    if (pb == NULL)
        return -1;
    uint32_t start = ch->pb_put - GR_DISPATCH_WORDS;
    uint32_t n = 0;
    pb[n++] = ga10b_pb_inc(GA10B_SUBCH_COMPUTE, GA10B_MTHD_SET_OBJECT, 1);
    pb[n++] = GA10B_CLASS_COMPUTE_B;
    pb[n++] = ga10b_pb_inc(GA10B_SUBCH_COMPUTE, GA10B_CP_SET_SHADER_LOCAL_MEMORY_A, 2);
    pb[n++] = (uint32_t)(gr.local_gpu_va >> 32) & 0x1ffffu;
    pb[n++] = (uint32_t)gr.local_gpu_va;
    pb[n++] = ga10b_pb_inc(GA10B_SUBCH_COMPUTE,
                 GA10B_CP_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A, 3);
    pb[n++] = (uint32_t)((uint64_t)GR_LOCAL_MEM_SIZE >> 32) & 0xffu;
    pb[n++] = GR_LOCAL_MEM_SIZE;
    pb[n++] = gr.sm_total & 0x1ffu;      /* C.MAX_SM_COUNT */
    pb[n++] = ga10b_pb_inc(GA10B_SUBCH_COMPUTE, GA10B_CP_SEND_PCAS_A, 1);
    pb[n++] = (uint32_t)(qmd_gpu_va >> 8);
    pb[n++] = ga10b_pb_inc(GA10B_SUBCH_COMPUTE, GA10B_CP_SEND_SIGNALING_PCAS2_B, 1);
    pb[n++] = GA10B_CP_PCAS2_INV_COPY_SCHED;

    ga10b_chan_push(ch, start, n);
    if (!ga10b.quiet)
        klog("ga10b: compute dispatch qmd=%llx sem=%u sm=%u\n",
             (unsigned long long)qmd_gpu_va, payload, gr.sm_total);
    ga10b_chan_submit(ch);
    if (!ga10b.hw)
        return ga10b_chan_wait_sem(ch, payload, GR_SEM_TRIES);
    /* a launch normally retires in well under a scheduler tick, and
     * sleeping costs at least one: spin on the semaphore for a few
     * milliseconds before falling back to the sleeping poll */
    volatile uint32_t* sem = (volatile uint32_t*)ch->sem_va;
    uint64_t t0 = 0, now = 0;
    kernel_tic_nsec(&t0);
    for (uint32_t spin = 0; ; spin++) {
        if (*sem >= payload)
            return 0;
        if ((spin & 63u) == 63u) {
            kernel_tic_nsec(&now);
            if (now - t0 > GR_SEM_SPIN_NS)
                break;
        }
    }
    /* polled gr isr: a pending GR interrupt holds the method stream, so log
     * the trapped method and ack it (the offending method is dropped) */
    for (uint32_t i = 0, logged = 0; i < GR_SEM_TRIES / 100u; i++) {
        if (ga10b_chan_wait_sem(ch, payload, 0) == 0)
            return 0;
        uint32_t intr = ga10b_rd(GA10B_GR_INTR);
        if (intr != 0u && logged < 16u) {
            uint32_t ta = ga10b_rd(0x00400704u);
            klog("ga10b: gr intr=%08x subch=%u mthd=%04x data=%08x.%08x "
                 "class_error=%08x fe_hww=%08x/%08x exc=%08x\n", intr,
                 (ta >> 16) & 7u, ta & 0x3ffcu, ga10b_rd(0x0040070cu),
                 ga10b_rd(0x00400708u), ga10b_rd(0x00400110u),
                 ga10b_rd(0x00404000u), ga10b_rd(0x004041b0u),
                 ga10b_rd(0x00400108u));
            ga10b_wr(GA10B_GR_INTR, intr);
            logged++;
        }
        ga10b_delay_us(1000);
    }
    ga10b_gr_launch_postmortem();
    return ga10b_chan_wait_sem(ch, payload, 1);
}

/* placeholder-QMD variant (dry-run trace + host tests) */
int ga10b_compute_dispatch(ga10b_chan_t* ch, uint64_t shader_gpu_va) {
    if (ga10b.hw && shader_gpu_va == 0) {
        klog("ga10b: no shader program for hw dispatch\n");
        return -1;
    }
    uint32_t qmd[64];
    memset(qmd, 0, sizeof(qmd));
    /* GR_QMD_REGS is a placeholder SASS reg count; real launches build the
     * QMD from the cubin (ga10b_compute.c). */
    ga10b_qmd_minimal(qmd, shader_gpu_va, ch->sem_gpu_va, ch->sem_val + 1u,
                      GR_QMD_REGS);
    return ga10b_compute_launch(ch, qmd);
}