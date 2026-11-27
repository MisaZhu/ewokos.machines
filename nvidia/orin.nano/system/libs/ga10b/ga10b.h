#ifndef GA10B_H
#define GA10B_H

/*
 * GA10B (Tegra234 / Orin Nano) GPU bring-up module -- standalone skeleton.
 *
 * Port target of the L4T nvgpu rel-36 GA10B path (see
 * machines/nvidia/orin.nano/docs/gpu-accel-research.md).  Retail T234 has
 * priv_sec_en burned: every falcon must boot LS through ACR-GSP + WPR2
 * using the official NVIDIA signed/encrypted blobs.  This driver only
 * ferries the blobs and replicates the register handshake; it never
 * touches keys.
 *
 * NOTHING here runs at boot.  ga10bd is a CLI tool: each bring-up stage
 * is an explicit subcommand, and hardware access additionally requires
 * --hw.  Without --hw every stage prints its planned register sequence
 * (dry-run) so the code can be reviewed before any board exposure.
 *
 * Milestones:
 *   M0  power -> detect -> priv/bus -> ACR-GSP/WPR2 -> GMMU -> FIFO -> CE
 *   M1  PMU RTOS -> GR (golden ctx, FECS/GPCCS via ACR) -> compute QMD
 *
 * Register offsets/values are transcribed from OE4T linux-nvgpu rel-36
 * include/nvgpu/hw/ga10b/hw_*_ga10b.h (MIT), cited per definition.
 */

#include <stdint.h>
#include "ga10b_va.h"
#include <ga10b/ga10b_dev.h>
#include <stddef.h>

#include "ga10b_fw.h"

#define GA10B_BAR0_PHY      0x17000000ULL
#define GA10B_BAR0_SIZE     0x01000000ULL   /* 16 MB */
#define GA10B_BAR0_VA       0x6b000000ULL   /* free window in orin VA map */

/* BPMP ids (tegra234 bpmp-abi / clk/reset/powergate headers, verified in
 * gpu-accel-research.md). */
#define GA10B_PG_GPU        35
#define GA10B_RESET_GPU     19
#define GA10B_CLK_GPUSYS    304
#define GA10B_CLK_GPC0      41
#define GA10B_CLK_GPC1      236
#define GA10B_CLK_NAFLL_GPUSYS 428
#define GA10B_CLK_FUSE      40

typedef struct {
    volatile uint32_t* bar0;     /* mapped BAR0, NULL until ga10b_map() */
    int mapped;
    int powered;                 /* BPMP pg/clk/reset done */
    int hw;                      /* 0 = dry-run: log instead of MMIO */
    uint32_t chip_id;            /* PMC boot_0 implementation field */
    int priv_sec_en;             /* fuse: 1 = secured silicon (LS only) */
    uint32_t gpc_disable;        /* opt_gpc fuse mask (bit=1 floorswept) */
    uint32_t gpc_count;          /* live GPCs after floorsweep */
    uint64_t gpusys_hz;
    uint32_t skip;               /* GA10B_SKIP_*: bring-up A/B switches */
    int quiet;                   /* no per-map / per-launch logging */
} ga10b_dev_t;

/* --skip MASK: leave out one nvgpu GR init step to attribute a root cause */
#define GA10B_SKIP_GFX_NONCTX  0x01u   /* gfx non-ctx netlist lists */
#define GA10B_SKIP_PES_MASK    0x02u   /* live PES mask (errata 200075440) */
#define GA10B_SKIP_SMID_TABLE  0x04u   /* identity sm-id order, no PES remap */
#define GA10B_SKIP_GR_GATING   0x08u   /* GR SLCG/BLCG prod lists */
#define GA10B_SKIP_SMC_TS      0x10u   /* SMC arbiter timestamp prod */
#define GA10B_SKIP_LATE_SETUP  0x20u   /* setup_hw before (not after) falcons */
#define GA10B_SKIP_ENG_GFX     0x40u   /* eng_config compute|i2m only */
#define GA10B_SKIP_PMU_IRQ     0x80u   /* no PMU ISR emulation (irqsclr + dmesg ack) */
#define GA10B_SKIP_PMU_DMESG   0x100u  /* PMU ISR emulation without the dmesg ack */
#define GA10B_SKIP_GFX_CTX     0x200u  /* MIG filter: no gfx regs in sw_ctx_load */

extern ga10b_dev_t ga10b;

/* ---- bring-up stages (ga10b_dev.c), nvgpu_finalize_poweron order ---- */
int ga10b_stage_init(void);                 /* power..bus..cg */
int ga10b_stage_acr(const char* fw_dir);    /* + FB/FIFO/GR enable + ACR-GSP */
int ga10b_stage_pmu(const char* fw_dir);    /* + PMU RTOS + FECS/GPCCS sigs */
int ga10b_stage_gr(const char* fw_dir);     /* + ctxsw start + golden ctx */
void ga10b_diag_dump(void);                 /* runlist/PBDMA/GR state */

/* ---- register access (ga10b_power.c) ----
 * In dry-run mode reads return 0 and writes only log.  Poll helpers
 * return -1 on timeout (dry-run: immediate success). */
uint32_t ga10b_rd(uint32_t off);
void     ga10b_wr(uint32_t off, uint32_t val);
void     ga10b_wr_mask(uint32_t off, uint32_t mask, uint32_t val);
int      ga10b_poll(uint32_t off, uint32_t mask, uint32_t want, uint32_t tries);
int      ga10b_map(void);
void     ga10b_delay_us(uint32_t us);

/* ---- power / detect (ga10b_power.c) ---- */
int ga10b_power_on(void);        /* bpmp pg35 + clks + reset19 deassert */
int ga10b_power_off(void);
int ga10b_detect(void);          /* PMC boot_0 -> chip_id, ga10b check */
int ga10b_fuse_read(void);       /* priv_sec_en (needs clocks on) */
int ga10b_gpc_clk_init(void);    /* opt_gpc fuse -> conditional GPC1CLK */
int ga10b_priv_ring_init(void);  /* ringmaster + sys ringstations */
int ga10b_bus_init(void);        /* pri bus timeout etc. */
int ga10b_cg_early_init(void);   /* timer/CE SLCG prods (no ELPG write) */
int ga10b_mc_intr_mask(void);    /* mask every FUNC top vector (polled) */

/* TOP device table (runlist/engines discovery) */
typedef struct {
    uint32_t type;           /* GA10B_DEVTYPE_* (row0 type_enum) */
    uint32_t inst_id;        /* row0 instance_id */
    uint32_t pri_base;       /* engine pri aperture */
    uint32_t reset_id;
    uint32_t runlist_base;   /* runlist engine pri aperture */
    uint32_t rleng_id;
    int      is_engine;
} ga10b_devinfo_t;
typedef struct {
    uint32_t count;
    ga10b_devinfo_t dev[32];
} ga10b_devtable_t;
int ga10b_device_table(ga10b_devtable_t* t);

/* Pure parser of the raw TOP device_info2 rows (ga10b_top_parse_next_dev /
 * ga10b_top_parse_device), host-unit-tested.  Empty rows are skipped; a
 * device spans up to 3 rows linked by the chain bit (b31) and row 0 must
 * have it set.
 *   row0: type_enum 30:24 | instance_id 23:16 | fault_id 10:0
 *   row1: is_engine b30 | device_pri_base 25:8 (<<8) | reset_id 7:0
 *   row2: runlist_pri_base 25:10 (<<10) | rleng_id 1:0 */
static inline void ga10b_devtable_parse(const uint32_t* row, uint32_t nrows,
                                        ga10b_devtable_t* t) {
    uint32_t tok = 0;
    t->count = 0;
    while (tok < nrows && t->count < 32u) {
        uint32_t r[3] = { 0, 0, 0 };
        r[0] = row[tok++];
        if (r[0] == 0)
            continue;
        if (tok >= nrows)
            break;
        for (uint32_t j = 1; j < 3u && tok < nrows; j++) {
            r[j] = row[tok++];
            if (!(r[j] & 0x80000000u))
                break;
        }
        if (!(r[0] & 0x80000000u))
            continue;                    /* broken single-row entry */
        ga10b_devinfo_t* d = &t->dev[t->count++];
        d->type = (r[0] >> 24) & 0x7fu;
        d->inst_id = (r[0] >> 16) & 0xffu;
        d->reset_id = r[1] & 0xffu;
        d->pri_base = ((r[1] >> 8) & 0x3ffffu) << 8;
        d->is_engine = (int)((r[1] >> 30) & 1u);
        d->rleng_id = r[2] & 0x3u;
        d->runlist_base = ((r[2] >> 10) & 0xffffu) << 10;
    }
}

/* first device of `type` with instance `inst`, or NULL */
static inline const ga10b_devinfo_t* ga10b_device_find(
        const ga10b_devtable_t* t, uint32_t type, uint32_t inst) {
    for (uint32_t i = 0; i < t->count; i++)
        if (t->dev[i].type == type && t->dev[i].inst_id == inst)
            return &t->dev[i];
    return NULL;
}
int ga10b_engine_enable(uint32_t reset_id, int enable);  /* mc_device_enable */
int ga10b_engine_reset(uint32_t reset_id);   /* disable then enable */
int ga10b_fb_ltc_early_init(void);           /* pre-ACR FB/LTC CG prods */
int ga10b_fb_init_hw(void);                  /* sysmem flush + mmu debug bufs */

/* ---- firmware (ga10b_fw.c) ---- */
typedef struct {
    void*    va;                 /* uncached DMA pool VA */
    uint64_t phy;                /* physical base for GPU DMA */
    uint32_t size;
} ga10b_fw_t;

#define GA10B_FW_DIR_DEFAULT "/fs/nvidia/ga10b"
int  ga10b_fw_load(const char* dir, const char* name, ga10b_fw_t* out);
void ga10b_fw_free(ga10b_fw_t* fw);

/* full M0+M1 firmware set, loaded into DMA memory + validated */
typedef struct {
    ga10b_fw_t acr_manifest, acr_text, acr_data;
    ga10b_fw_t pmu_desc, pmu_image, pmu_sig;
    ga10b_fw_t fecs_enc, fecs_sig, gpccs_enc, gpccs_sig;
    ga10b_fw_t net[4];
    ga10b_pmu_ucode_desc_t pmu;      /* parsed desc */
} ga10b_fw_set_t;

int  ga10b_fw_set_load(const char* dir, ga10b_fw_set_t* set);
void ga10b_fw_set_free(ga10b_fw_set_t* set);
/* load+validate+report every blob; works in dry-run (no MMIO needed) */
int  ga10b_fw_inventory(const char* dir);
/* plain DMA buffer (page tables, pushbuffers, instance blocks) */
int  ga10b_dma_alloc(uint32_t size, uint32_t align, void** va, uint64_t* phy);
void ga10b_dma_free(void* va);

/* ---- device-lifetime memory (ga10b_pmem.c) ----
 * The GPU cannot be power-cycled from here (BPMP denies pg_off / reset), so
 * the PMU RTOS booted by the first GPU process keeps running after that
 * process is gone.  Everything the RTOS references must therefore outlive
 * the process: it lives in one pinned contiguous segment that later
 * processes re-attach.  The header page carries what they need to talk to
 * the running RTOS; objects are recorded as offsets, never as addresses.
 * Dry-run uses a private scratch block instead of the live segment. */
#define GA10B_PMEM_SIZE     0x00200000u
#define GA10B_PMEM_MAGIC    0x47413142u       /* 'GA1B' */
#define GA10B_PMEM_VERSION  1u
typedef struct {
    uint32_t magic, version;
    uint32_t size;                 /* segment bytes */
    uint32_t cursor;               /* bump allocator offset */
    uint64_t phy_base;
    uint32_t owner_pid, owner_uuid; /* process driving the GPU; uuid 0 = none */
    struct {
        uint32_t valid;            /* the objects below belong to a booted RTOS */
        uint32_t inst_off, ss_off, trace_off, args_off, dump_off;
        uint64_t ss_gpu_va, trace_gpu_va;
        uint32_t ipc_ready, lpq_phy_idx, sw_off, sw_size, seq_id, wpr_done;
    } pmu;
} ga10b_pmem_hdr_t;

ga10b_pmem_hdr_t* ga10b_pmem_hdr(void);     /* attaches on first use */
int      ga10b_pmem_alloc(uint32_t size, uint32_t align, void** va,
                          uint64_t* phy);
uint32_t ga10b_pmem_off(const void* va);
void*    ga10b_pmem_va(uint32_t off, uint32_t size);   /* NULL if out of range */
uint64_t ga10b_pmem_phy(uint32_t off);
void     ga10b_pmem_reset(void);            /* drop every object, keep the owner */
/* OS layer (ga10b_fw.c): the segment itself and process identity */
int  ga10b_os_pmem_map(uint32_t size, void** va, uint64_t* phy);
void ga10b_os_self(uint32_t* pid, uint32_t* uuid);
int  ga10b_os_alive(uint32_t pid, uint32_t uuid);

/* ---- falcon / riscv (ga10b_falcon.c) ---- */
typedef struct {
    uint32_t base;               /* falcon reg base inside BAR0 */
    int      riscv;              /* GSP/PMU-next are RISC-V cores */
} ga10b_falcon_t;

int ga10b_falcon_dma_load(const ga10b_falcon_t* f, uint64_t phy,
                          uint32_t imem_off, const void* imem, uint32_t imem_sz,
                          uint32_t dmem_off, const void* dmem, uint32_t dmem_sz);
/* PIO load (ACR-GSP path): IMEM secure-tagged, falcon2 has no imem tags */
int ga10b_falcon_pio_load(const ga10b_falcon_t* f,
                          uint32_t imem_off, const void* imem, uint32_t imem_sz,
                          int sec,
                          uint32_t dmem_off, const void* dmem, uint32_t dmem_sz);
int ga10b_falcon_start(const ga10b_falcon_t* f, uint32_t bootvec);
int ga10b_falcon_wait_idle(const ga10b_falcon_t* f, uint32_t tries);
int ga10b_falcon_wait_halted(const ga10b_falcon_t* f, uint32_t tries);
int ga10b_falcon_reset(const ga10b_falcon_t* f);
uint32_t ga10b_falcon_mailbox(const ga10b_falcon_t* f, int idx);
void ga10b_falcon_mbox_wr(const ga10b_falcon_t* f, int idx, uint32_t val);
uint32_t ga10b_falcon_hwcfg2(const ga10b_falcon_t* f);
/* RISC-V BROM (priscv aperture = falcon base + 0x1000) */
void ga10b_priscv_set_bcr(const ga10b_falcon_t* f);       /* bcr_ctrl = 0x11 */
int ga10b_priscv_brom_config(const ga10b_falcon_t* f, uint64_t code_pa,
                             uint64_t data_pa, uint64_t manifest_pa);
int ga10b_priscv_boot(const ga10b_falcon_t* f, uint64_t bootvec, uint32_t tries);

/* ---- ACR / GSP / WPR (ga10b_acr.c) ----
 * WPR carveout is READ-ONLY on Tegra: MB1/MB2/BPMP lock it before the
 * GPU driver runs; ga10b ACR uses the WPR1 region (ga10b_fb_read_wpr_info). */
int ga10b_wpr_read(uint64_t* base, uint32_t* size);
int ga10b_acr_init(const char* fw_dir);     /* load acr-gsp blobs, build desc */
int ga10b_acr_done(void);                   /* previous successful ACR this power cycle */
int ga10b_acr_boot_gsp(void);               /* HS boot on GSP RISC-V */
int ga10b_acr_bootstrap_ls(uint32_t falcon_id); /* FECS/GPCCS via ACR cmd */

/* ---- PMU (ga10b_pmu.c) ---- */
int ga10b_pmu_init(const char* fw_dir);     /* load desc/image/sig */
int ga10b_pmu_boot(void);                   /* RTOS boot (M1; CE-only skips) */
/* FBQ IPC: PMU INIT message + ACR RPC bootstrapping FECS/GPCCS (writes HS
 * signatures).  ga10b_pmu_gr_done() reads the FECS signature register. */
int ga10b_pmu_ipc_init(void);
int ga10b_pmu_bootstrap_gr(void);
int ga10b_pmu_gr_done(void);

/* ---- GMMU (ga10b_mm.c) ----
 * 5-level ver2 page tables (hw_gmmu_ga10b.h, gmmu_ga10b_fusa.c):
 *   PDE3[48:47] -> PDE2[46:38] -> PDE1[37:29] -> PDE0[28:21] -> PTE[20:12]
 * All tables are 4KB DMA pages; child tables allocated on demand from a
 * fixed pool.  Mappings use 4KB small pages, sys_ncoh aperture (uncached
 * DMA pool), volatile, privileged, read-write. */
#define GA10B_MM_MAX_TABLES  4096   /* x 2MB of mapped VA per PTE table;
                                       LLM arenas (3GB+) need ~1500+ tables */
#define GA10B_MM_MAX_FREE    256    /* returned bump ranges kept for reuse */
typedef struct {
    void*    pdb_va;   uint64_t pdb_phy;    /* PDE3 root (level 0) */
    uint64_t va_next;                        /* bump VA allocator cursor */
    ga10b_va_range_t free_va[GA10B_MM_MAX_FREE];
    uint32_t nfree;                          /* ranges in free_va */
    struct { void* va; uint64_t phy; } tab[GA10B_MM_MAX_TABLES];
    uint32_t ntab;                           /* child tables allocated */
    uint32_t aperture;                       /* PDE/PTE sysmem aperture: 6 ncoh, 4 coh */
    int      persist;                        /* tables in device-lifetime memory */
} ga10b_gmmu_t;

int      ga10b_gmmu_init(ga10b_gmmu_t* mm);
/* VM whose page tables outlive the process (never destroyed) */
int      ga10b_gmmu_init_persist(ga10b_gmmu_t* mm);
uint64_t ga10b_gmmu_map(ga10b_gmmu_t* mm, uint64_t phy, uint32_t size);
uint64_t ga10b_gmmu_map_attr(ga10b_gmmu_t* mm, uint64_t phy, uint32_t size,
                             int cacheable);
int      ga10b_l2_invalidate(void);
int      ga10b_l2_flush(void);
/* fixed-VA variants for caller-managed windows: map (or remap) a range,
 * and invalidate one (its page tables are kept for reuse) */
int      ga10b_gmmu_map_at(ga10b_gmmu_t* mm, uint64_t va, uint64_t phy,
                           uint32_t size, int cacheable);
void     ga10b_gmmu_unmap(ga10b_gmmu_t* mm, uint64_t va, uint32_t size);
void     ga10b_gmmu_unmap_free(ga10b_gmmu_t* mm, uint64_t va, uint32_t size);
void     ga10b_gmmu_destroy(ga10b_gmmu_t* mm);
void     ga10b_gmmu_dump(const ga10b_gmmu_t* mm, uint64_t va);

/* ---- FIFO / channels (ga10b_fifo.c) ---- */
struct ga10b_fifo_rl;
typedef struct {
    uint32_t chid;
    uint32_t tsgid;
    struct ga10b_fifo_rl* rl;      /* engine runlist (CE != GR) */
    void*    inst_va;   uint64_t inst_phy;  /* instance block (4KB) */
    void*    userd_va;  uint64_t userd_phy; /* USERD page */
    void*    gpfifo_va; uint64_t gpfifo_phy;
    uint64_t gpfifo_gpu_va;                  /* GMMU-mapped ring VA */
    uint32_t gpfifo_entries;                 /* power of two */
    uint32_t gpfifo_put;
    void*    pb_va;     uint64_t pb_phy;    /* pushbuffer ring */
    uint64_t pb_gpu_va;
    uint32_t pb_words;                       /* capacity in u32 */
    uint32_t pb_put;                         /* next free word */
    void*    sem_va;    uint64_t sem_phy;   /* semaphore payload page */
    uint64_t sem_gpu_va;
    uint32_t sem_val;                        /* last released value */
    void*    rq_va;     uint64_t rq_phy;    /* engine method buffer (runqueue) */
    uint64_t rq_gpu_va;
    /* GR context (ga10b_gr_ctx_prepare): golden image copy + patch + hdr */
    void*    gr_ctx_va;   uint64_t gr_ctx_phy;   uint64_t gr_ctx_gpu_va;
    void*    gr_patch_va; uint64_t gr_patch_phy; uint64_t gr_patch_gpu_va;
    void*    gr_hdr_va;   uint64_t gr_hdr_phy;   uint64_t gr_hdr_gpu_va;
    ga10b_gmmu_t* mm;
} ga10b_chan_t;

int ga10b_fifo_init(void);                  /* runlist/doorbell setup */
/* Take every channel off the hardware, from hardware state alone (no driver
 * bookkeeping needed).  reset_engines=0: the caller still owns the channels'
 * memory, contexts are preempted out.  reset_engines=1: the channels belong
 * to a process that is gone, the engines are reset instead of saved. */
void ga10b_fifo_quiesce(int reset_engines);
/* retained bar2 host-VM page tables (runqueue translation; lives for the
 * driver lifetime like the runlist buffers, never freed per channel) */
uint32_t ga10b_fifo_bar2_ntab(void);
/* devtype/inst select the engine runlist: (GRAPHICS,0) for compute,
 * (LCE,2) for async CE (LCE0/1 are grcopy, not directly schedulable). */
int ga10b_chan_create(ga10b_gmmu_t* mm, ga10b_chan_t* ch,
                      uint32_t devtype, uint32_t inst);
int ga10b_chan_bind_engine(ga10b_chan_t* ch, uint32_t subch, uint32_t class_id);
int ga10b_chan_submit(ga10b_chan_t* ch);    /* doorbell kick */
int ga10b_chan_wait_sem(ga10b_chan_t* ch, uint32_t val, uint32_t tries);
void ga10b_chan_destroy(ga10b_chan_t* ch);
/* pushbuffer append helpers used by ce/gr: reserve words, then publish
 * pb[start..start+words) as a GPFIFO entry (USERD gp_put + barrier) */
uint32_t* ga10b_pb_reserve(ga10b_chan_t* ch, uint32_t words);
void ga10b_chan_push(ga10b_chan_t* ch, uint32_t pb_start, uint32_t words);

/* pushbuffer encoder: pure logic, host-unit-testable (ga10b_fifo.h) */

/* ---- CE (ga10b_ce.c) ---- */
int ga10b_ce_init(void);                    /* engine reset, no falcon dep */
/* src/dst are GPU VAs from ga10b_gmmu_map (virtual-address CE methods) */
int ga10b_ce_blit(ga10b_chan_t* ch,
                  uint64_t src_va, uint32_t src_pitch,
                  uint64_t dst_va, uint32_t dst_pitch,
                  uint32_t width_bytes, uint32_t height);

/* ---- GR / compute (ga10b_gr.c, M1 skeleton) ---- */
/* nvgpu_gr_enable_hw: GR engine + GPC reset, fifo access, non-ctx netlist
 * lists, falcon scrub wait.  MUST run before the ACR boots FECS/GPCCS. */
int ga10b_gr_enable_hw(const char* fw_dir);
int ga10b_gr_init(const char* fw_dir);      /* FECS/GPCCS start + golden ctx */
/* load golden image into a channel ctx + header page + RAMIN GR pointers */
int ga10b_gr_ctx_prepare(ga10b_chan_t* ch);
int ga10b_compute_dispatch(ga10b_chan_t* ch, uint64_t shader_gpu_va);
int ga10b_compute_launch(ga10b_chan_t* ch, const uint32_t* qmd_words);
/* queued launches: submit without waiting, drain once (ga10b_gr.c) */
#define GA10B_QUEUE_SLOTS  64u      /* launches between two drains */
#define GA10B_QUEUE_CB0    1024u    /* largest constant bank 0 a slot holds */
int ga10b_compute_queue_slot(ga10b_chan_t* ch, uint32_t cb0_size,
                             void** cb0_va, uint64_t* cb0_gpu_va);
int ga10b_compute_queue(ga10b_chan_t* ch, const uint32_t* qmd_words);
uint32_t ga10b_compute_queued(void);
void ga10b_compute_timestamps(int enable);
int ga10b_compute_timestamp(uint32_t slot, uint64_t* timestamp);
int ga10b_compute_drain(ga10b_chan_t* ch, uint32_t* retired);
void ga10b_gr_launch_postmortem(void);
/* ga10b_compute.c: nvcc sm_87 cubin modules.  A module keeps one uploaded
 * code + constant-bank set per kernel in the given VM; launches are
 * synchronous (release0 semaphore).  args[i] points at kernel parameter i. */
int ga10b_buf_alloc(ga10b_gmmu_t* mm, uint32_t size, void** va,
                    uint64_t* gpu_va, int cacheable);
void* ga10b_file_read(const char* path, uint32_t* size);
ga10b_module_t* ga10b_module_load(ga10b_gmmu_t* mm, const void* image,
                                  uint32_t size);
int ga10b_kernel_launch(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                        const uint32_t grid[3], const uint32_t block[3],
                        void* const* args);
int ga10b_kernel_launch_dyn(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                            const uint32_t grid[3], const uint32_t block[3],
                            void* const* args, uint32_t dyn_smem);
/* compiler-generated compute kernel (nvcompiler cubin): uniform blob + SSBO
 * base VAs + texture descriptors are filled into constant bank 0 at the
 * nv_meta.h layout offsets */
int ga10b_kernel_queue_dyn(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                           const uint32_t grid[3], const uint32_t block[3],
                           void* const* args, uint32_t dyn_smem);
int ga10b_kernel_launch_compute(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                                const uint32_t grid[3], const uint32_t block[3],
                                const uint32_t* unif, uint32_t unif_dwords,
                                const uint64_t* ssbo_va, uint32_t nssbo,
                                const uint32_t* texdesc,
                                uint32_t texdesc_dwords);
/* self-tests: projects/kuda vadd, and the portablegl geometry kernels
 * (fill + xform + raster) against their CPU reference */
int ga10b_compute_vadd(ga10b_chan_t* ch, const char* cubin_path);
int ga10b_pgl_selftest(ga10b_chan_t* ch, const char* cubin_path);
/* read-only: hex-dump FECS/GPCCS IMEM+DMEM for offline disassembly */
void ga10b_gr_falcon_memdump(void);

#endif