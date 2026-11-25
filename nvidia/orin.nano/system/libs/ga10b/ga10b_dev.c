/*
 * GA10B bring-up stages and the process-wide compute device.
 *
 * The stages are the nvgpu_finalize_poweron order split at the points the
 * bring-up CLI exposes (init / acr / pmu / gr).  ga10b_dev_open() runs the
 * whole chain once and keeps one VM + one compute channel for the lifetime
 * of the process: the host for it is g2dd (through libbsp's bsp_g3d), the
 * same way libadreno lives inside g2dd on the Adreno machine.
 */
#include <stdlib.h>
#include <string.h>
#include <ewoksys/klog.h>

#include "ga10b.h"
#include "ga10b_regs.h"
#include "ga10b_fifo.h"
#include <ga10b/ga10b_dev.h>

ga10b_dev_t ga10b;

int ga10b_stage_init(void) {
    if (ga10b_power_on() != 0)
        return -1;
    if (ga10b_map() != 0)
        return -1;
    if (ga10b_detect() != 0)
        return -1;
    if (ga10b_fuse_read() != 0)
        return -1;
    if (ga10b_gpc_clk_init() != 0)
        return -1;
    /* nvgpu masks the interrupt tree before touching the priv ring */
    if (ga10b_mc_intr_mask() != 0)
        return -1;
    if (ga10b_priv_ring_init() != 0)
        return -1;
    if (ga10b_bus_init() != 0)
        return -1;
    if (ga10b_cg_early_init() != 0)
        return -1;
    return 0;
}

/* The GPU keeps its state when a process exits (it cannot be power-cycled
 * from here) while every buffer that process gave it is freed.  So exactly
 * one process drives the GPU at a time: it records itself in the
 * device-lifetime header, takes its channels off the hardware when it exits,
 * and a later process may only take over once it is gone. */
static int dev_claimed;

static void dev_release(void) {
    if (ga10b.mapped)
        ga10b_fifo_quiesce(0);
    ga10b_pmem_hdr_t* h = ga10b_pmem_hdr();
    if (h != NULL)
        h->owner_uuid = 0;
}

static int dev_claim(void) {
    if (dev_claimed || !ga10b.hw)
        return 0;
    ga10b_pmem_hdr_t* h = ga10b_pmem_hdr();
    if (h == NULL)
        return -1;
    uint32_t pid, uuid;
    ga10b_os_self(&pid, &uuid);
    if (!(h->owner_pid == pid && h->owner_uuid == uuid) &&
        ga10b_os_alive(h->owner_pid, h->owner_uuid)) {
        klog("ga10b: GPU is in use by pid %u; one GPU process at a time\n",
             h->owner_pid);
        return -1;
    }
    h->owner_pid = pid;
    h->owner_uuid = uuid;
    dev_claimed = 1;
    atexit(dev_release);
    return 0;
}

/* nvgpu_finalize_poweron order: FB/LTC + MM hw setup, FIFO and
 * nvgpu_gr_enable_hw all precede acr_construct_execute ("For acr to boot,
 * nvgpu_init_fb_support and init_mm_support is required").
 *
 * When an earlier process already ran the ACR this power cycle (one-shot),
 * the GPU is taken over nvgpu_gr_reset style instead: whatever that process
 * left on the hardware is quiesced, the GR engine is reset like on a first
 * bring-up, and stage_pmu has the still-running PMU RTOS bootstrap
 * FECS/GPCCS again.  The rest of the chain is then identical for a first
 * and a later process. */
int ga10b_stage_acr(const char* fw_dir) {
    static int done;
    if (done)
        return 0;
    if (dev_claim() != 0)
        return -1;
    if (ga10b_power_on() != 0)
        return -1;
    if (ga10b_map() != 0)
        return -1;
    int reuse = ga10b_acr_done();
    if (reuse) {
        klog("ga10bd: ACR already completed this power cycle -- take over "
             "(quiesce FIFO, reset GR, PMU re-bootstraps FECS/GPCCS)\n");
        ga10b_fifo_quiesce(1);
    }
    if (ga10b_stage_init() != 0)
        return -1;
    if (ga10b_fb_ltc_early_init() != 0)
        return -1;
    if (ga10b_fb_init_hw() != 0)
        return -1;
    if (ga10b_fifo_init() != 0)
        return -1;
    if (ga10b_gr_enable_hw(fw_dir) != 0)
        return -1;
    if (!reuse) {
        if (ga10b_acr_init(fw_dir) != 0)
            return -1;
        if (ga10b_acr_boot_gsp() != 0)
            return -1;
    }
    done = 1;
    return 0;
}

/* ACR-managed LS-PMU: boot the PMU RTOS, then RPC-bootstrap FECS/GPCCS (the
 * PMU writes their HS signatures).  Must run after stage_acr (ACR placed the
 * PMU image in WPR) and before gr_init starts the ctxsw falcons. */
int ga10b_stage_pmu(const char* fw_dir) {
    if (ga10b_pmu_init(fw_dir) != 0)
        return -1;
    if (ga10b_pmu_boot() != 0)
        return -1;
    if (ga10b_pmu_bootstrap_gr() != 0)
        return -1;
    return 0;
}

/* GR chain up to the golden context: FECS/GPCCS are bootstrapped by the PMU
 * RTOS (ACR-managed LS-PMU, lazy_bootstrap=1), which writes their HS
 * signatures before gr_init starts them. */
int ga10b_stage_gr(const char* fw_dir) {
    if (ga10b_stage_acr(fw_dir) != 0)
        return -1;
    if (ga10b_stage_pmu(fw_dir) != 0)
        return -1;
    return ga10b_gr_init(fw_dir);
}

void ga10b_diag_dump(void) {
    if (!ga10b.hw || !ga10b.mapped) {
        klog("ga10bd: diag needs --hw + mapped BAR0\n");
        return;
    }
    ga10b_devtable_t t;
    if (ga10b_device_table(&t) != 0)
        return;
    uint32_t seen[8];
    uint32_t nseen = 0;
    for (uint32_t i = 0; i < t.count; i++) {
        const ga10b_devinfo_t* d = &t.dev[i];
        if (!d->is_engine || d->runlist_base == 0)
            continue;
        uint32_t rl = d->runlist_base;
        int dup = 0;
        for (uint32_t j = 0; j < nseen; j++)
            if (seen[j] == rl)
                dup = 1;
        if (dup)
            continue;
        seen[nseen++] = rl;
        uint32_t cc = ga10b_rd(rl + GA10B_RL_CHANNEL_CONFIG);
        uint32_t chram = ((cc >> 4) & 0xfffffffu) << 4;
        klog("ga10bd: rl@%x submit_info=%08x engstat0=%08x pbcfg0=%08x "
             "pbcfg1=%08x chram0=%08x chram1=%08x\n", rl,
             ga10b_rd(rl + GA10B_RL_SUBMIT_INFO),
             ga10b_rd(rl + GA10B_RL_ENGINE_STATUS1(0)),
             ga10b_rd(rl + GA10B_RL_PBDMA_CONFIG(0)),
             ga10b_rd(rl + GA10B_RL_PBDMA_CONFIG(1)),
             chram ? ga10b_rd(chram) : 0,
             chram ? ga10b_rd(chram + 4) : 0);
    }
    for (uint32_t i = 0; i < 4; i++)
        klog("ga10bd: pbdma%u status=%08x intr=%08x gp_get=%08x gp_base=%08x "
             "mthd0=%08x\n", i,
             ga10b_rd(GA10B_PBDMA_STATUS(i)), ga10b_rd(GA10B_PBDMA_INTR_0(i)),
             ga10b_rd(GA10B_PBDMA_GP_GET(i)), ga10b_rd(GA10B_PBDMA_GP_BASE(i)),
             ga10b_rd(GA10B_PBDMA_METHOD0(i)));
    for (uint32_t i = 0; i < 4; i++)
        klog("ga10bd: pbdma%u sig=%08x subdev=%08x pbhdr=%08x hdrsh=%08x "
             "get=%08x.%08x put=%08x.%08x gpfetch=%08x\n", i,
             ga10b_rd(GA10B_PBDMA_SIGNATURE(i)),
             ga10b_rd(GA10B_PBDMA_SUBDEVICE(i)),
             ga10b_rd(GA10B_PBDMA_PB_HEADER(i)),
             ga10b_rd(GA10B_PBDMA_HDR_SHADOW(i)),
             ga10b_rd(GA10B_PBDMA_GET_HI(i)), ga10b_rd(GA10B_PBDMA_GET(i)),
             ga10b_rd(GA10B_PBDMA_PUT_HI(i)), ga10b_rd(GA10B_PBDMA_PUT(i)),
             ga10b_rd(GA10B_PBDMA_GP_FETCH(i)));
    for (uint32_t i = 0; i < 4; i++)
        klog("ga10bd: pbdma%u intr1=%08x cfg0=%08x sched=%08x\n", i,
             ga10b_rd(GA10B_PBDMA_INTR_1(i)),
             ga10b_rd(GA10B_PBDMA_CFG0(i)),
             ga10b_rd(GA10B_PBDMA_STATUS_SCHED(i)));
    for (uint32_t i = 0; i < 4; i++)
        klog("ga10bd: ce%u bind=%08x launcherr=%08x intr=%08x engctl=%08x\n", i,
             ga10b_rd(GA10B_CE_LCE_BIND_STATUS(i)),
             ga10b_rd(GA10B_CE_LCE_LAUNCHERR(i)),
             ga10b_rd(GA10B_CE_INTR_STATUS(i)),
             ga10b_rd(GA10B_CE_LCE_ENGCTL(i)));
    klog("ga10bd: gr status=%08x fecs cpuctl=%08x cur_ctx=%08x mbox0=%08x "
         "mbox1=%08x gpccs cpuctl=%08x\n",
         ga10b_rd(GA10B_GR_STATUS), ga10b_rd(GA10B_FECS_CPUCTL),
         ga10b_rd(GA10B_FECS_CURRENT_CTX),
         ga10b_rd(GA10B_FECS_CTXSW_MBOX(0)),
         ga10b_rd(GA10B_FECS_CTXSW_MBOX(1)),
         ga10b_rd(GA10B_GPCCS_FALCON_BASE + GA10B_FALCON_CPUCTL));
    klog("ga10bd: mc_device_enable=%08x mmu_fault_status=%08x info=%08x\n",
         ga10b_rd(GA10B_MC_DEVICE_ENABLE),
         ga10b_rd(GA10B_FB_MMU_FAULT_STATUS),
         ga10b_rd(GA10B_FB_MMU_FAULT_INFO));
    klog("ga10bd: mmu_fault_addr=%08x.%08x inst=%08x\n",
         ga10b_rd(GA10B_FB_MMU_FAULT_ADDR_HI),
         ga10b_rd(GA10B_FB_MMU_FAULT_ADDR_LO),
         ga10b_rd(GA10B_FB_MMU_FAULT_INST_LO));
    /* ucode error 0x98 check: GR 0x40a000/0x40a100 must equal 0xaf88.
     * error path copies actual values to 0x420a00/0x420b00. */
    klog("ga10bd: sig chk 40a000=%08x 40a004=%08x 40a100=%08x 40a104=%08x\n",
         ga10b_rd(0x40a000), ga10b_rd(0x40a004),
         ga10b_rd(0x40a100), ga10b_rd(0x40a104));
    klog("ga10bd: sig rec 420a00=%08x 420b00=%08x 420600=%08x mbox6=%08x\n",
         ga10b_rd(0x420a00), ga10b_rd(0x420b00),
         ga10b_rd(0x420600), ga10b_rd(0x409818));
    for (uint32_t a = 0x409000; a < 0x40b000; a += 4) {
        uint32_t v = ga10b_rd(a);
        if (v == 0xaf88)
            klog("ga10bd: sig 0xaf88 found @%08x\n", a);
    }
    klog("ga10bd: fe blk 40a200=%08x 40a300=%08x 40a220=%08x 40a260=%08x "
         "40a790=%08x 40a798=%08x\n",
         ga10b_rd(0x40a200), ga10b_rd(0x40a300), ga10b_rd(0x40a220),
         ga10b_rd(0x40a260), ga10b_rd(0x40a790), ga10b_rd(0x40a798));
    /* falcon io = (GR-0x409000)<<6: check io 0xa000=GR 0x409280,
     * io 0xa200=0x409288; gate io 0x9000=0x409240; ext-access io 0x1ca00=0x409728 */
    klog("ga10bd: fecs sig 409240=%08x 409280=%08x 409284=%08x 409288=%08x "
         "40928c=%08x\n",
         ga10b_rd(0x409240), ga10b_rd(0x409280), ga10b_rd(0x409284),
         ga10b_rd(0x409288), ga10b_rd(0x40928c));
    klog("ga10bd: fecs ext 409728=%08x 40972c=%08x 409730=%08x mbox10=%08x "
         "mbox11=%08x\n",
         ga10b_rd(0x409728), ga10b_rd(0x40972c), ga10b_rd(0x409730),
         ga10b_rd(0x409828), ga10b_rd(0x40982c));
}


/* ---- process-wide compute device ---- */

/* Client buffers live in a VA window of fixed-size slots, apart from the
 * bump-allocated driver buffers: a slot is remapped in place when its
 * buffer is evicted, so the window (and its page tables) never grows. */
#define DEV_MAP_SLOTS  64u
#define DEV_SLOT_SPAN  0x04000000ULL      /* 64MB: a 4K ARGB surface is 33MB */
#define DEV_SLOT_BASE  0x100000000ULL

static struct {
    int state;                 /* 0 = not opened, 1 = ready, -1 = failed */
    ga10b_gmmu_t mm;
    ga10b_chan_t ch;
    struct { uint64_t phy; uint32_t size; uint32_t used; } map[DEV_MAP_SLOTS];
    uint32_t tick;             /* LRU clock */
} dev;

int ga10b_dev_open(const char* fw_dir) {
    if (dev.state != 0)
        return dev.state == 1 ? 0 : -1;
    dev.state = -1;
    ga10b.hw = 1;
    if (ga10b_stage_gr(fw_dir != NULL ? fw_dir : GA10B_FW_DIR_DEFAULT) != 0)
        return -1;
    if (ga10b_gmmu_init(&dev.mm) != 0 || ga10b_fifo_init() != 0)
        return -1;
    /* the GR runlist also carries the grcopy LCEs and RAMFC target marks the
     * CE ctx valid: with them held in reset PBDMA raises eng_reset and stalls
     * (nvgpu_ce_init_support always runs before the first channel) */
    if (ga10b_ce_init() != 0)
        return -1;
    if (ga10b_chan_create(&dev.mm, &dev.ch, GA10B_DEVTYPE_GRAPHICS, 0) != 0)
        return -1;
    ga10b.quiet = 1;
    dev.state = 1;
    return 0;
}

int ga10b_dev_ready(void) {
    return dev.state == 1;
}

/* GPU VA of a physically contiguous client buffer.  Mappings are cached by
 * base address (canvases are long-lived shm segments reused every frame);
 * when every slot is taken the least recently used one is remapped.  Only
 * call between launches: every mapping a launch uses must be requested
 * after the previous launch retired, and at most DEV_MAP_SLOTS per launch.
 * Returns 0 for an unmappable buffer. */
uint64_t ga10b_dev_map(uint64_t phy, uint32_t size) {
    if (dev.state != 1 || phy == 0 || size == 0 || (phy & 0xfffu) != 0 ||
        size > DEV_SLOT_SPAN)
        return 0;
    uint32_t victim = 0;
    for (uint32_t i = 0; i < DEV_MAP_SLOTS; i++) {
        if (dev.map[i].phy == phy && dev.map[i].size >= size) {
            dev.map[i].used = ++dev.tick;
            return DEV_SLOT_BASE + i * DEV_SLOT_SPAN;
        }
        if (dev.map[i].used < dev.map[victim].used)
            victim = i;
    }
    uint64_t va = DEV_SLOT_BASE + victim * DEV_SLOT_SPAN;
    if (dev.map[victim].size > size)
        ga10b_gmmu_unmap(&dev.mm, va, dev.map[victim].size);
    dev.map[victim].phy = 0;
    dev.map[victim].size = 0;
    if (ga10b_gmmu_map_at(&dev.mm, va, phy, size, 1) != 0)
        return 0;
    dev.map[victim].phy = phy;
    dev.map[victim].size = size;
    dev.map[victim].used = ++dev.tick;
    return va;
}

int ga10b_dev_alloc(uint32_t size, void** va, uint64_t* gpu_va) {
    if (dev.state != 1)
        return -1;
    return ga10b_buf_alloc(&dev.mm, size, va, gpu_va, 1);
}

/* The buffers of ga10b_dev_map / ga10b_dev_alloc are cached in the GPU L2:
 * bracket every span of launches with to_gpu (after the CPU finished
 * writing their inputs) and to_cpu (before it reads their outputs). */
int ga10b_dev_sync_to_gpu(void) {
    return ga10b_l2_invalidate();
}

int ga10b_dev_sync_to_cpu(void) {
    return ga10b_l2_flush();
}

ga10b_module_t* ga10b_dev_module(const void* cubin, uint32_t size) {
    if (dev.state != 1)
        return NULL;
    return ga10b_module_load(&dev.mm, cubin, size);
}

int ga10b_dev_launch(ga10b_module_t* m, int kernel, const uint32_t grid[3],
                     const uint32_t block[3], void* const* args) {
    if (dev.state != 1)
        return -1;
    return ga10b_kernel_launch(&dev.ch, m, kernel, grid, block, args);
}

int ga10b_dev_launch_compute(ga10b_module_t* m, int kernel,
                             const uint32_t grid[3], const uint32_t block[3],
                             const uint32_t* unif, uint32_t unif_dwords,
                             const uint64_t* ssbo_va, uint32_t nssbo) {
    if (dev.state != 1)
        return -1;
    return ga10b_kernel_launch_compute(&dev.ch, m, kernel, grid, block,
                                       unif, unif_dwords, ssbo_va, nssbo);
}
