/*
 * FIFO: runlist submission + GPFIFO channels for GA10B.
 *
 * Sequence transcribed from nvgpu rel-36:
 *   ga10b_device_table / runlist regs  (hw_runlist_ga10b.h)
 *   fifo_ga10b_fusa.c ga10b_init_fifo_setup_hw + usermode_ga10b_fusa.c
 *   ramfc_ga10b_fusa.c / pbdma_ga10b_fusa.c  (instance block init)
 *   channel.c: setup_ramfc -> update_runlist -> bind (CHRAM enable)
 *   channel_ga10b_fusa.c: teardown = disable -> CHRAM clear -> new runlist
 *   runlist_fifo_ga10b_fusa.c submit path + usermode doorbell
 *
 * Runlists: ga10b runlist0 hosts gr0 + grcopy0/1 (LCE0/1).  grcopy0/1
 * "can't schedule work directly" (nvgpu ramfc_ga10b_fusa.c comment), so
 * user CE channels run on LCE2/LCE3 which own separate runlists.  CHRAM,
 * doorbell id and runlist buffers are per-runlist; each channel gets its
 * own TSG and the runlist always carries EVERY live channel, rebuilt into
 * the inactive one of two buffers on each create/destroy.
 * All GPU-visible rings live in the uncached sys_dma pool and are mapped
 * through the channel GMMU (sys_ncoh + volatile).
 *
 * Not implemented: TSG preempt before channel removal (callers only
 * destroy idle channels), runlist/pbdma interrupt handling (polled).
 * ga10b_fifo_quiesce() is the whole-device teardown: it runs when a process
 * lets go of the GPU and when the next one takes it over.
 *
 * Dry-run: page tables / instance blocks / runlist buffer are built for
 * real in DMA memory; only the MMIO handshake is logged, not executed.
 */

#include <string.h>

#include <ewoksys/klog.h>

#include "ga10b.h"
#include "ga10b_regs.h"
#include "ga10b_fifo.h"

#define GPFIFO_ENTRIES   1024u        /* 8KB ring, log2 = 10 */
#define GPFIFO_LOG2      10u
#define PB_WORDS         4096u        /* 16KB pushbuffer */
#define FIFO_MAX_CH      16u
#define FIFO_MAX_RL      4u
#define RL_BUF_SIZE      4096u        /* 16B entries: [TSG, CH] per channel */
#define RL_SUBMIT_TRIES  1000u
#define RL_PREEMPT_TRIES 2000u        /* x 100us */
#define RL_MAX_CHANNELS  2048u
/* nvgpu runqueue size = ((9+1+3)*num_pce + 2) * 27 * 5, page-aligned
 * (tsg_gv11b_fusa.c); 16KB covers ga10b's 3-PCE layout with margin. */
#define RUNQUEUE_SIZE    16384u

typedef struct ga10b_fifo_rl {
    uint32_t rl_base;                 /* runlist PRI base (BAR0 offset) */
    uint32_t rl_id;                   /* doorbell_config id */
    uint32_t chram_base;              /* CHRAM BAR0 offset */
    uint32_t rleng_id;                /* runlist engine id (ramfc target) */
    uint32_t next_chid;
    uint32_t next_tsgid;
    void*    rl_buf_va[2];            /* active / next, swapped per submit */
    uint64_t rl_buf_phy[2];
    uint32_t rl_cur;
    uint32_t nlive;
    struct {
        uint32_t chid, tsgid;
        uint64_t inst_phy, userd_phy;
    } live[FIFO_MAX_CH];
} ga10b_fifo_rl_t;

static struct {
    int ready;
    uint32_t nrl;
    ga10b_fifo_rl_t rl[FIFO_MAX_RL];
} fifo;

/* Host (bar2) VM: PBDMA writes fetched methods into each channel's runqueue
 * through this page directory, NOT the channel's own GMMU (nvgpu maps
 * eng_method_buffers in g->mm.bar2.vm and binds it via bus_bar2_block).
 * One shared VM for all channels' runqueues; bound once at fifo_init. */
static ga10b_gmmu_t bar2_mm;
static void* bar2_inst_va;
static uint64_t bar2_inst_phy;
static int bar2_ready;

static void gpu_dsb(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

static int bar2_setup(void) {
    if (bar2_ready)
        return 0;
    if (ga10b_gmmu_init(&bar2_mm) != 0)
        return -1;
    if (ga10b_dma_alloc(4096, 4096, &bar2_inst_va, &bar2_inst_phy) != 0)
        return -1;
    uint32_t* w = (uint32_t*)bar2_inst_va;
    w[GA10B_RAMIN_PDB_LO] = ga10b_ramin_pdb_lo(bar2_mm.pdb_phy);
    w[GA10B_RAMIN_PDB_HI] = (uint32_t)(bar2_mm.pdb_phy >> 32);
    gpu_dsb();
    /* gp10b_bus_bar2_bind: instance-block ptr, virtual mode, sys_ncoh */
    ga10b_wr(GA10B_BUS_BAR2_BLOCK,
             GA10B_BUS_BAR2_MODE_VIRTUAL | GA10B_BUS_BAR2_TARGET_SYS_NCOH |
             (uint32_t)((bar2_inst_phy >> 12) & 0xfffffffu));
    if (ga10b.hw &&
        ga10b_poll(GA10B_BUS_BIND_STATUS,
                   GA10B_BUS_BIND_BAR2_PENDING | GA10B_BUS_BIND_BAR2_OUTSTANDING,
                   0, 1000) != 0) {
        klog("ga10b: bar2 bind timeout\n");
        return -1;
    }
    bar2_ready = 1;
    klog("ga10b: bar2 host vm pdb=%llx inst=%llx bound\n",
         (unsigned long long)bar2_mm.pdb_phy,
         (unsigned long long)bar2_inst_phy);
    return 0;
}

static ga10b_fifo_rl_t* rl_find(uint32_t rl_base) {
    for (uint32_t i = 0; i < fifo.nrl; i++)
        if (fifo.rl[i].rl_base == rl_base)
            return &fifo.rl[i];
    return NULL;
}

static int rl_setup(ga10b_fifo_rl_t* rl, uint32_t rl_base, uint32_t rleng_id) {
    rl->rl_base = rl_base;
    rl->rleng_id = rleng_id;
    uint32_t cc = ga10b_rd(rl_base + GA10B_RL_CHANNEL_CONFIG);
    rl->chram_base = ((cc >> 4) & 0xfffffffu) << 4;
    uint32_t db = ga10b_rd(rl_base + GA10B_RL_DOORBELL_CONFIG);
    rl->rl_id = (db >> 16) & 0xffffu;
    if (ga10b.hw && rl->chram_base == 0) {
        klog("ga10b: runlist@%x channel_config chram offset 0\n", rl_base);
        return -1;
    }
    klog("ga10b: runlist base=%08x id=%u chram=%08x rleng=%u\n",
         rl->rl_base, rl->rl_id, rl->chram_base, rl->rleng_id);
    for (uint32_t i = 0; i < 2; i++)
        if (ga10b_dma_alloc(RL_BUF_SIZE, 4096,
                            &rl->rl_buf_va[i], &rl->rl_buf_phy[i]) != 0)
            return -1;
    /* per-runlist CG prods (offsets relative to the runlist pri base) */
    ga10b_wr(rl_base + GA10B_RL_SLCG, 0);
    ga10b_wr(rl_base + GA10B_RL_BLCG, 0x4042);
    /* ga10b_usermode_setup_hw: enable the usermode doorbell for GFID 0 */
    ga10b_wr(rl_base + GA10B_RL_VIRT_CH_CFG(0),
             ga10b_rd(rl_base + GA10B_RL_VIRT_CH_CFG(0)) |
             GA10B_RL_VIRT_CH_HW_INIT | GA10B_RL_VIRT_CH_PENDING_EN);
    /* ga10b_fifo_config_userd_writeback_timer */
    ga10b_wr(rl_base + GA10B_RL_USERD_WRITEBACK,
             GA10B_RL_USERD_WRITEBACK_VAL);
    return 0;
}

/* One runlist per schedulable engine: gr0 (compute/graphics) and the async
 * LCEs (inst >= 2).  LCE0/1 share gr0's runlist and are not directly
 * schedulable; gr1 is unused. */
static int rl_wanted(const ga10b_devinfo_t* d) {
    if (!d->is_engine || d->runlist_base == 0)
        return 0;
    return (d->type == GA10B_DEVTYPE_GRAPHICS && d->inst_id == 0) ||
           (d->type == GA10B_DEVTYPE_LCE && d->inst_id >= 2);
}

/* gv11b_fifo_recover order, driven by hardware state only so it also works
 * for channels this process never created:
 *   scheduler off -> CHRAM disable every channel in use -> [engine reset] ->
 *   runlist preempt -> CHRAM clear -> scheduler on -> empty runlist.
 * With reset_engines the preempt comes AFTER the reset, unlike nvgpu: the
 * contexts belong to a process that is gone, and a preempt on a loaded
 * engine would have FECS save them into memory that is no longer theirs. */
void ga10b_fifo_quiesce(int reset_engines) {
    static uint8_t used[FIFO_MAX_RL][RL_MAX_CHANNELS / 8u];
    ga10b_devtable_t t;
    if (!ga10b.hw || ga10b_device_table(&t) != 0)
        return;
    memset(used, 0, sizeof(used));
    uint32_t base[FIFO_MAX_RL];
    uint32_t nrl = 0;
    for (uint32_t i = 0; i < t.count && nrl < FIFO_MAX_RL; i++) {
        if (!rl_wanted(&t.dev[i]))
            continue;
        int dup = 0;
        for (uint32_t j = 0; j < nrl; j++)
            if (base[j] == t.dev[i].runlist_base)
                dup = 1;
        if (!dup)
            base[nrl++] = t.dev[i].runlist_base;
    }

    uint32_t found = 0;
    for (uint32_t r = 0; r < nrl; r++) {
        ga10b_wr(base[r] + GA10B_RL_SCHED_DISABLE, 1u);
        uint32_t cc = ga10b_rd(base[r] + GA10B_RL_CHANNEL_CONFIG);
        uint32_t chram = ((cc >> 4) & 0xfffffffu) << 4;
        uint32_t nch = 1u << (cc & 0xfu);
        if (chram == 0)
            continue;
        if (nch > RL_MAX_CHANNELS)
            nch = RL_MAX_CHANNELS;
        for (uint32_t c = 0; c < nch; c++) {
            uint32_t v = ga10b_rd(chram + GA10B_CHRAM_CHANNEL(c));
            if ((v & (GA10B_CHRAM_IN_USE | GA10B_CHRAM_BUSY |
                      GA10B_CHRAM_ON_PBDMA | GA10B_CHRAM_ON_ENG)) == 0u)
                continue;
            klog("ga10b: quiesce rl@%x chan%u chram=%08x\n", base[r], c, v);
            ga10b_wr(chram + GA10B_CHRAM_CHANNEL(c), GA10B_CHRAM_DISABLE);
            used[r][c / 8u] |= (uint8_t)(1u << (c % 8u));
            found++;
        }
    }

    if (reset_engines) {
        for (uint32_t i = 0; i < t.count; i++) {
            const ga10b_devinfo_t* d = &t.dev[i];
            if ((d->type == GA10B_DEVTYPE_GRAPHICS && d->inst_id == 0) ||
                d->type == GA10B_DEVTYPE_LCE)
                ga10b_engine_reset(d->reset_id);
        }
    }

    for (uint32_t r = 0; r < nrl; r++) {
        ga10b_wr(base[r] + GA10B_RL_PREEMPT, 0u);       /* type = runlist */
        uint32_t i;
        for (i = 0; i < RL_PREEMPT_TRIES; i++) {
            if ((ga10b_rd(base[r] + GA10B_RL_PREEMPT) &
                 GA10B_RL_PREEMPT_RUNLIST_PENDING) == 0u)
                break;
            ga10b_delay_us(100);
        }
        if (i == RL_PREEMPT_TRIES)
            klog("ga10b: quiesce rl@%x preempt still pending (%08x)\n",
                 base[r], ga10b_rd(base[r] + GA10B_RL_PREEMPT));

        uint32_t cc = ga10b_rd(base[r] + GA10B_RL_CHANNEL_CONFIG);
        uint32_t chram = ((cc >> 4) & 0xfffffffu) << 4;
        for (uint32_t c = 0; chram != 0 && c < RL_MAX_CHANNELS; c++)
            if (used[r][c / 8u] & (1u << (c % 8u)))
                ga10b_wr(chram + GA10B_CHRAM_CHANNEL(c), GA10B_CHRAM_CLEAR);

        ga10b_wr(base[r] + GA10B_RL_SCHED_DISABLE, 0u);
        ga10b_wr(base[r] + GA10B_RL_SUBMIT, 0u);        /* no entries */
        if (ga10b_poll(base[r] + GA10B_RL_SUBMIT_INFO,
                       GA10B_RL_SUBMIT_INFO_PENDING, 0, RL_SUBMIT_TRIES) != 0)
            klog("ga10b: quiesce rl@%x empty runlist still pending\n",
                 base[r]);
    }
    klog("ga10b: fifo quiesced (%u runlists, %u channels%s)\n", nrl, found,
         reset_engines ? ", engines reset" : "");
}

int ga10b_fifo_init(void) {
    if (fifo.ready)
        return 0;

    ga10b_devtable_t t;
    if (ga10b_device_table(&t) == 0) {
        for (uint32_t i = 0; i < t.count && fifo.nrl < FIFO_MAX_RL; i++) {
            const ga10b_devinfo_t* d = &t.dev[i];
            if (!rl_wanted(d) || rl_find(d->runlist_base) != NULL)
                continue;
            if (rl_setup(&fifo.rl[fifo.nrl], d->runlist_base, d->rleng_id) != 0)
                return -1;
            fifo.nrl++;
        }
    }
    if (fifo.nrl == 0) {
        if (ga10b.hw) {
            klog("ga10b: no runlist in the TOP device table\n");
            return -1;
        }
        /* dry-run: TOP reads return 0, no device table */
        if (rl_setup(&fifo.rl[0], GA10B_RUNLIST0_BASE, 0) != 0)
            return -1;
        fifo.nrl = 1;
        klog("ga10b: runlist base not discovered, fallback %08x (dry-run)\n",
             GA10B_RUNLIST0_BASE);
    }
    if (bar2_setup() != 0)
        return -1;
    fifo.ready = 1;
    return 0;
}

uint32_t ga10b_fifo_bar2_ntab(void) {
    return bar2_mm.ntab;
}

/* rebuild [TSG, CH] x nlive into the inactive buffer and submit it.  The
 * scheduler is left alone: nvgpu never touches sched_disable here. */
static int runlist_update(ga10b_fifo_rl_t* rl) {
    uint32_t next = rl->rl_cur ^ 1u;
    uint32_t* rlb = (uint32_t*)rl->rl_buf_va[next];
    for (uint32_t i = 0; i < rl->nlive; i++) {
        ga10b_rl_tsg(rlb + 8u * i, rl->live[i].tsgid, 1);
        ga10b_rl_ch(rlb + 8u * i + 4u, rl->live[i].chid,
                    rl->live[i].inst_phy, rl->live[i].userd_phy);
    }
    gpu_dsb();

    uint32_t base = rl->rl_base;
    uint32_t count = 2u * rl->nlive;
    if (count != 0) {
        uint64_t phy = rl->rl_buf_phy[next];
        ga10b_wr(base + GA10B_RL_SUBMIT_BASE_LO,
                 (uint32_t)(((phy >> 10) & 0x3fffffu) << 10) |
                 GA10B_RL_TARGET_SYS_NCOH);
        ga10b_wr(base + GA10B_RL_SUBMIT_BASE_HI,
                 (uint32_t)((phy >> 32) & 0xffu));
    }
    ga10b_wr(base + GA10B_RL_SUBMIT, count & 0xffffu);   /* offset 0 */
    if (ga10b_poll(base + GA10B_RL_SUBMIT_INFO,
                   GA10B_RL_SUBMIT_INFO_PENDING, 0, RL_SUBMIT_TRIES) != 0) {
        klog("ga10b: runlist submit pending timeout\n");
        return -1;
    }
    rl->rl_cur = next;
    return 0;
}

/* keyed by instance block: unique, and valid for half-built channels */
static int live_find(ga10b_fifo_rl_t* rl, const ga10b_chan_t* ch) {
    for (uint32_t i = 0; i < rl->nlive; i++)
        if (rl->live[i].inst_phy == ch->inst_phy &&
            rl->live[i].chid == ch->chid)
            return (int)i;
    return -1;
}

static void chan_init_instance(ga10b_chan_t* ch) {
    uint32_t* w = (uint32_t*)ch->inst_va;
    ga10b_fifo_rl_t* rl = ch->rl;

    /* RAMFC (words 0..127) */
    w[GA10B_RAMFC_SIGNATURE]   = GA10B_RAMFC_SIG_VALID;
    w[GA10B_RAMFC_ACQUIRE]     = GA10B_RAMFC_ACQUIRE_VAL;
    w[GA10B_RAMFC_GP_BASE]     = ga10b_gp_base(ch->gpfifo_gpu_va);
    w[GA10B_RAMFC_GP_BASE_HI]  = ga10b_gp_base_hi(ch->gpfifo_gpu_va,
                                                  GPFIFO_LOG2);
    w[GA10B_RAMFC_PB_HEADER]   = GA10B_RAMFC_PB_HEADER_VAL;
    w[GA10B_RAMFC_PB_COUNT]    = 0;
    w[GA10B_RAMFC_SUBDEVICE]   = GA10B_RAMFC_SUBDEVICE_VAL;
    w[GA10B_RAMFC_TARGET]      = ga10b_ramfc_target(rl->rleng_id);
    w[GA10B_RAMFC_HCE_CTRL]    = GA10B_RAMFC_HCE_PRIV;
    w[GA10B_RAMFC_CONFIG]      = GA10B_RAMFC_CONFIG_VAL;
    /* polled completion: top interrupt tree stays masked, vector unused */
    w[GA10B_RAMFC_INTR_NOTIFY] = ga10b_ramfc_intr_notify(0);
    w[GA10B_RAMFC_SET_CH_INFO] = ga10b_ramfc_ch_info(0, ch->chid);

    /* RAMIN (words 128+): engine page directory = channel GMMU root */
    w[GA10B_RAMIN_PDB_LO]   = ga10b_ramin_pdb_lo(ch->mm->pdb_phy);
    w[GA10B_RAMIN_PDB_HI]   = (uint32_t)(ch->mm->pdb_phy >> 32);
    w[GA10B_RAMIN_FW_MAGIC] = GA10B_RAMIN_FW_MAGIC_VAL;
    w[GA10B_RAMIN_WFI_VEID] = 0;
    /* runqueue: PBDMA->engine method handoff (gv11b ramin w136/137) */
    w[GA10B_RAMIN_ENG_MTHD_BUF_LO] = (uint32_t)ch->rq_gpu_va;
    w[GA10B_RAMIN_ENG_MTHD_BUF_HI] = (uint32_t)(ch->rq_gpu_va >> 32);
}

int ga10b_chan_create(ga10b_gmmu_t* mm, ga10b_chan_t* ch,
                      uint32_t devtype, uint32_t inst) {
    if (!fifo.ready) {
        klog("ga10b: chan_create before fifo_init\n");
        return -1;
    }
    memset(ch, 0, sizeof(*ch));

    /* pick the engine's runlist; dry-run has a single fallback runlist */
    ga10b_fifo_rl_t* rl = &fifo.rl[0];
    if (ga10b.hw) {
        ga10b_devtable_t t;
        const ga10b_devinfo_t* d = NULL;
        if (ga10b_device_table(&t) == 0)
            d = ga10b_device_find(&t, devtype, inst);
        if (d == NULL || d->runlist_base == 0 ||
            (rl = rl_find(d->runlist_base)) == NULL) {
            klog("ga10b: no runlist for devtype %u inst %u\n", devtype, inst);
            return -1;
        }
    }
    if (rl->nlive >= FIFO_MAX_CH) {
        klog("ga10b: too many live channels (%u)\n", rl->nlive);
        return -1;
    }
    ch->rl = rl;
    ch->mm = mm;
    ch->gpfifo_entries = GPFIFO_ENTRIES;
    ch->pb_words = PB_WORDS;

    if (ga10b_dma_alloc(4096, 4096, &ch->inst_va, &ch->inst_phy) != 0 ||
        ga10b_dma_alloc(4096, 4096, &ch->userd_va, &ch->userd_phy) != 0 ||
        ga10b_dma_alloc(GPFIFO_ENTRIES * 8, 4096,
                        &ch->gpfifo_va, &ch->gpfifo_phy) != 0 ||
        ga10b_dma_alloc(PB_WORDS * 4, 4096, &ch->pb_va, &ch->pb_phy) != 0 ||
        ga10b_dma_alloc(4096, 4096, &ch->sem_va, &ch->sem_phy) != 0 ||
        ga10b_dma_alloc(RUNQUEUE_SIZE, 4096, &ch->rq_va, &ch->rq_phy) != 0)
        goto fail;
    memset(ch->rq_va, 0, RUNQUEUE_SIZE);

    ch->gpfifo_gpu_va = ga10b_gmmu_map(mm, ch->gpfifo_phy,
                                       GPFIFO_ENTRIES * 8);
    ch->pb_gpu_va = ga10b_gmmu_map(mm, ch->pb_phy, PB_WORDS * 4);
    ch->sem_gpu_va = ga10b_gmmu_map(mm, ch->sem_phy, 4096);
    /* runqueue lives in the bar2 host VM, not the channel VM (see bar2_setup) */
    ch->rq_gpu_va = ga10b_gmmu_map(&bar2_mm, ch->rq_phy, RUNQUEUE_SIZE);
    if (ch->gpfifo_gpu_va == 0 || ch->pb_gpu_va == 0 ||
        ch->sem_gpu_va == 0 || ch->rq_gpu_va == 0) {
        klog("ga10b: chan gmmu map failed\n");
        goto fail;
    }

    ch->chid = rl->next_chid++;
    ch->tsgid = rl->next_tsgid++;
    chan_init_instance(ch);
    gpu_dsb();

    /* nvgpu order: ramfc -> runlist update -> bind (CHRAM enable) */
    rl->live[rl->nlive].chid = ch->chid;
    rl->live[rl->nlive].tsgid = ch->tsgid;
    rl->live[rl->nlive].inst_phy = ch->inst_phy;
    rl->live[rl->nlive].userd_phy = ch->userd_phy;
    rl->nlive++;
    if (runlist_update(rl) != 0)
        goto fail;
    ga10b_wr(rl->chram_base + GA10B_CHRAM_CHANNEL(ch->chid),
             GA10B_CHRAM_ENABLE);

    klog("ga10b: chan%u tsg%u rl@%x inst=%llx userd=%llx gp=%llx pb=%llx "
         "sem=%llx\n", ch->chid, ch->tsgid, rl->rl_base,
         (unsigned long long)ch->inst_phy,
         (unsigned long long)ch->userd_phy,
         (unsigned long long)ch->gpfifo_gpu_va,
         (unsigned long long)ch->pb_gpu_va,
         (unsigned long long)ch->sem_gpu_va);
    return 0;
fail:
    ga10b_chan_destroy(ch);
    return -1;
}

/* append room for `words` pushbuffer words; wraps the ring when full */
uint32_t* ga10b_pb_reserve(ga10b_chan_t* ch, uint32_t words) {
    if (words > ch->pb_words)
        return NULL;
    if (ch->pb_put + words > ch->pb_words)
        ch->pb_put = 0;
    uint32_t* p = (uint32_t*)ch->pb_va + ch->pb_put;
    ch->pb_put += words;
    return p;
}

/* publish pb[start..start+words) as one GPFIFO entry + USERD gp_put */
void ga10b_chan_push(ga10b_chan_t* ch, uint32_t pb_start, uint32_t words) {
    uint32_t* e = (uint32_t*)ch->gpfifo_va + 2 * ch->gpfifo_put;
    ga10b_gpfifo_entry(e, ch->pb_gpu_va + (uint64_t)pb_start * 4, words);
    ch->gpfifo_put = (ch->gpfifo_put + 1) & (ch->gpfifo_entries - 1);
    ((uint32_t*)ch->userd_va)[GA10B_USERD_GP_PUT] = ch->gpfifo_put;
    gpu_dsb();
}

int ga10b_chan_bind_engine(ga10b_chan_t* ch, uint32_t subch,
                           uint32_t class_id) {
    uint32_t* pb = ga10b_pb_reserve(ch, 2);
    if (pb == NULL)
        return -1;
    uint32_t start = ch->pb_put - 2;
    pb[0] = ga10b_pb_inc(subch, GA10B_MTHD_SET_OBJECT, 1);
    pb[1] = class_id;
    ga10b_chan_push(ch, start, 2);
    return ga10b_chan_submit(ch);
}

int ga10b_chan_submit(ga10b_chan_t* ch) {
    gpu_dsb();
    ga10b_wr(GA10B_DOORBELL, GA10B_DOORBELL_TOKEN(ch->chid, ch->rl->rl_id));
    return 0;
}

int ga10b_chan_wait_sem(ga10b_chan_t* ch, uint32_t val, uint32_t tries) {
    if (!ga10b.hw) {
        klog("ga10b: [dry] wait_sem(%u) skipped (no GPU execution)\n", val);
        return 0;
    }
    volatile uint32_t* sem = (volatile uint32_t*)ch->sem_va;
    for (uint32_t i = 0; i < tries; i++) {
        if (*sem >= val)
            return 0;
        ga10b_delay_us(10);
    }
    if (*sem >= val)
        return 0;
    if (tries != 0u)
        klog("ga10b: sem timeout want=%u got=%u\n", val, *sem);
    return -1;
}

/* The channel must be idle.  Its memory stays referenced by the runlist
 * until the replacement runlist is accepted, so it is freed only after. */
void ga10b_chan_destroy(ga10b_chan_t* ch) {
    ga10b_fifo_rl_t* rl = ch->rl;
    int idx = (fifo.ready && rl != NULL && ch->inst_va != NULL) ?
              live_find(rl, ch) : -1;
    if (idx >= 0) {
        uint32_t chram = rl->chram_base + GA10B_CHRAM_CHANNEL(ch->chid);
        ga10b_wr(chram, GA10B_CHRAM_DISABLE);
        ga10b_wr(chram, GA10B_CHRAM_CLEAR);
        rl->nlive--;
        for (uint32_t i = (uint32_t)idx; i < rl->nlive; i++)
            rl->live[i] = rl->live[i + 1];
        if (runlist_update(rl) != 0)
            klog("ga10b: chan%u removal runlist update failed\n", ch->chid);
    }
    ga10b_dma_free(ch->inst_va);
    ga10b_dma_free(ch->userd_va);
    ga10b_dma_free(ch->gpfifo_va);
    ga10b_dma_free(ch->pb_va);
    ga10b_dma_free(ch->sem_va);
    ga10b_dma_free(ch->rq_va);
    ga10b_dma_free(ch->gr_ctx_va);
    ga10b_dma_free(ch->gr_patch_va);
    ga10b_dma_free(ch->gr_hdr_va);
    memset(ch, 0, sizeof(*ch));
}