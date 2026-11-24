/*
 * GA10B PMU next-core (RISC-V) RTOS bring-up -- M1 prerequisite.
 *
 * The PMU runs a signed RISC-V RTOS whose monitor is fetched by the core's
 * BROM from sysmem.  The driver arms the BROM DMA addresses (monitor code /
 * data / manifest, all 256B units), publishes the RTOS boot arguments via
 * the PMU mailboxes, starts the core, and waits for BROM pass + priv
 * lockdown release.  No keys are touched: BROM verifies the embedded
 * manifest with fuse-borne keys.
 *
 * Sources (OE4T linux-nvgpu rel-36, MIT):
 *   common/pmu/pmu_rtos_init.c        rtos_args_allocate/setup, boot flow,
 *                                     priv-lockdown-release poll
 *   hal/pmu/pmu_ga10b.c               ga10b_pmu_ns_nvriscv_bootstrap
 *                                     (brom_config with monitor offsets >>8)
 *   hal/falcon/falcon_ga10b_fusa.c    bootstrap / brom_config / is_priv_lockdown
 *   include/nvgpu/pmu.h               nv_pmu_boot_params + EXTMEM2 aperture
 *   include/nvgpu/pmu/pmuif/init.h    pmu_cmdline_args_v7
 *
 * On retail secured silicon (priv_sec_en=1, support_ls_pmu=true) nvgpu runs
 * the ACR-managed LS-PMU path: ACR HS authenticates the PMU next-core image
 * into WPR1 and arms the BROM DMA addresses to point into WPR; the driver
 * then applies the PMU SLCG/BLCG load-enable prods, publishes the RTOS boot
 * args, starts the core and waits for BROM pass + priv lockdown release
 * (nvgpu_pmu_rtos_init secure NEXT_CORE path).  ga10b_pmu_boot implements
 * that path.  The PMU RTOS subsequently bootstraps FECS/GPCCS (writing their
 * HS signatures) via the FBQ message-queue RPC (INIT_WPR_REGION then
 * BOOTSTRAP_GR_FALCONS).
 */

#include <string.h>
#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>

#include "ga10b.h"
#include "ga10b_regs.h"
#include "ga10b_mm.h"    /* GA10B_GMMU_PTE_APERTURE_COH */
#include "ga10b_fifo.h"   /* ga10b_ramin_pdb_lo */

static const ga10b_falcon_t pmu_flcn = { GA10B_PMU_FALCON_BASE, 1 };

/* ---- RTOS boot arguments, on-wire layout (nv_pmu_boot_params).
 * flcn_mem_desc_v0 uses falc_u64 {lo,hi}; the bootloader's first three
 * fields are frozen (boot_type/size/version). ---- */
typedef struct {
    uint32_t boot_type;
    uint16_t size;
    uint8_t  version;
    uint8_t  pad0;
} pmu_bootldr_params_t;            /* 8 */

typedef struct {
    uint64_t core_dump_phys;
    uint32_t core_dump_size;
    uint32_t pad0;
} pmu_rtos_params_t;               /* 16 */

typedef struct {
    pmu_bootldr_params_t bl;
    pmu_rtos_params_t rtos;
    uint32_t dummy[24];
} pmu_next_core_boot_params_t;     /* 120 */

typedef struct {
    ga10b_falc_u64_t address;
    uint32_t params;
} pmu_flcn_mem_desc_t;             /* 12 */

typedef struct {
    uint32_t cpu_freq_hz;
    pmu_flcn_mem_desc_t trace_buf;
    uint8_t  raise_priv_sec;
    uint8_t  pad0[3];
    pmu_flcn_mem_desc_t gc6_ctx;
    pmu_flcn_mem_desc_t gc6_bsod_ctx;
    pmu_flcn_mem_desc_t super_surface;
    uint32_t flags;
    uint32_t ctx_bind_addr;
} pmu_cmdline_args_v7_t;           /* 64 */

typedef struct {
    pmu_next_core_boot_params_t boot_params;
    pmu_cmdline_args_v7_t cmd_line_args;
} pmu_boot_params_t;               /* 184 */

_Static_assert(sizeof(pmu_bootldr_params_t) == 8u, "bootldr params size");
_Static_assert(sizeof(pmu_rtos_params_t) == 16u, "rtos params size");
_Static_assert(sizeof(pmu_next_core_boot_params_t) == 120u, "boot params size");
_Static_assert(sizeof(pmu_flcn_mem_desc_t) == 12u, "flcn mem desc size");
_Static_assert(sizeof(pmu_cmdline_args_v7_t) == 64u, "cmdline v7 size");
_Static_assert(sizeof(pmu_boot_params_t) == 184u, "pmu boot params size");

#define PMU_BOOT_TYPE_RM        0x4E56524Du   /* U32_BUILD('N','V','R','M') */
#define PMU_BOOTLDR_VERSION     1u
#define PMU_CORE_DUMP_SIZE      8192u
#define PMU_PWRCLK_HZ           204000000u    /* NVGPU_PWRCLK_RATE */
#define PMU_AMAP_EXTMEM2_START  0x8060000000000000ULL
#define PMU_PRIV_LOCKDOWN_BIT   (1u << 13)    /* hwcfg2 riscv_br_priv_lockdown */

/* ---- FBQ super surface (sysmem, mapped into the PMU VM) ----
 * layout: super_surface_priv.h -- ssmd[32]x16B@0, cmd_queues(2x16x2048)@0x200,
 * msg_queue(16x64)@0x10200, hdr(16B)@0x10600.  host writes hdr.address=gpu_va;
 * PMU fills ssmd + queues.  RPC transport for the ACR bootstrap commands. */
#define PMU_SS_SIZE           0x100000u       /* 1MB */
#define PMU_TRACE_SIZE        0x4000u         /* PMU_RTOS_TRACE_BUFSIZE (16KB) */
#define PMU_SS_CMD_Q_OFF      0x200u          /* + logical_id*0x8000 */
#define PMU_SS_MSG_Q_OFF      0x10200u
#define PMU_SS_HDR_OFF        0x10600u
#define PMU_CMD_ELEM_SIZE     2048u
#define PMU_MSG_ELEM_SIZE     64u
#define PMU_QUEUE_ELEMENTS    16u
#define PMU_LPQ_LOGICAL       1u              /* HPQ=0 LPQ=1 (cmd_queues index) */
#define PMU_SS_PARAMS         0x01100000u     /* (1MB & 0xFFFFFF)|(DMAIDX_VIRT=1<<24) */
/* queue head/tail registers (gv11b hw_pwr, ga10b reuses; full 32-bit values) */
#define PMU_CMD_HEAD(idx)     (0x0010a800u + 4u * (idx))
#define PMU_CMD_TAIL(idx)     (0x0010a820u + 4u * (idx))
#define PMU_MSG_HEAD          0x0010a4c8u     /* PMU writes, host reads */
#define PMU_MSG_TAIL          0x0010a4ccu     /* host writes, PMU reads */
/* RPC protocol (pmuif/cmn.h, rpc.h, acr.h) */
#define PMU_UNIT_ACR          0x0Au
#define PMU_UNIT_CMDMGMT      0x01u
#define PMU_UNIT_INIT_DGPU    0x0Fu           /* ga10b INIT msg unit_id */
#define PMU_RPC_CMD_ID        0x80u
#define PMU_CMD_CTRL_FLAGS    0x03u           /* STATUS|INTR */
#define PMU_RPC_INIT_WPR      0x00u
#define PMU_RPC_BOOTSTRAP_GR  0x03u
#define PMU_FECS_SIG_REG      0x00409280u     /* falcon io 0x280: 0xaf88 when signed */
#define PMU_FECS_SIG_VAL      0x0000af88u

static struct {
    int ready;
    ga10b_fw_t desc, image, sig;
    ga10b_pmu_ucode_desc_t d;
    void*    args_va;  uint64_t args_phy;
    void*    dump_va;  uint64_t dump_phy;
    /* PMU VM + super surface */
    ga10b_gmmu_t mm;
    void*    inst_va;  uint64_t inst_phy;
    void*    ss_va;    uint64_t ss_phy;  uint64_t ss_gpu_va;
    void*    trace_va; uint64_t trace_phy; uint64_t trace_gpu_va;
    /* FBQ IPC state (filled from the PMU INIT message) */
    int      ipc_ready;
    uint32_t lpq_phy_idx;      /* physical cmd queue index for LPQ registers */
    uint32_t sw_off, sw_size;  /* sw_managed_area (RPC heap) */
    uint32_t heap_next;        /* 32-aligned bump cursor */
    uint8_t  seq_id;
    int      gr_loaded;        /* FECS/GPCCS bootstrapped by PMU */
} st;

/* ---- nvriscv dmesg ring + host ISR emulation ----
 * falcon_debug.c: the RTOS logs into a ring at the end of PMU DMEM, metadata
 * {read,write,size,magic} in the last 16 bytes.  It publishes its write
 * offset in queue_head(7), raises swgen1, and the host answers with its read
 * offset in queue_tail(7).  gk20a_pmu_isr: irqsclr(stat & mask) first. */
#define PMU_DBG_QUEUE         7u
#define PMU_DBG_MAGIC         0xf007ba11u
#define PMU_DBG_MAX_SIZE      0x1000u         /* NV_RISCV_DMESG_BUFFER_SIZE */
#define PMU_DBG_META_SIZE     16u
#define PMU_IRQ_SWGEN1        0x80u
#define PMU_DMEMC_AINCR       (1u << 25)
#define PMU_DMEMC_ADDR_MASK   0x00fffffcu     /* offs 7:2 | blk 23:8 */

static struct {
    int      valid;
    uint32_t dmem_off, size, rd;
    uint32_t last_stat;
} dbg;

static void pmu_dmem_read(uint32_t off, uint32_t* dst, uint32_t words) {
    ga10b_wr(GA10B_PMU_FALCON_BASE + GA10B_FALCON_DMEMC(0),
             (off & PMU_DMEMC_ADDR_MASK) | PMU_DMEMC_AINCR);
    for (uint32_t i = 0; i < words; i++)
        dst[i] = ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_DMEMD(0));
}

/* print every complete (NUL-terminated) line between the host read offset and
 * the firmware write offset; commit publishes the new read offset. */
static void pmu_dmesg_drain(int commit) {
    static uint32_t ring[PMU_DBG_MAX_SIZE / 4u];
    if (!ga10b.hw)
        return;
    uint32_t wr = ga10b_rd(PMU_CMD_HEAD(PMU_DBG_QUEUE));
    if (!dbg.valid) {
        uint32_t dmem = ((ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_HWCFG)
                          >> 9) & 0x1ffu) << 8;
        uint32_t meta[4] = { 0, 0, 0, 0 };
        if (dmem > PMU_DBG_META_SIZE)
            pmu_dmem_read(dmem - PMU_DBG_META_SIZE, meta, 4);
        if (meta[3] != PMU_DBG_MAGIC || meta[2] == 0u ||
            meta[2] >= PMU_DBG_MAX_SIZE || (meta[2] & 3u) != 0u) {
            klog("ga10b: PMU dmesg meta invalid dmem=%x rd=%x wr=%x size=%x "
                 "magic=%08x wr_reg=%x rd_reg=%x\n", dmem, meta[0], meta[1],
                 meta[2], meta[3], wr, ga10b_rd(PMU_CMD_TAIL(PMU_DBG_QUEUE)));
            return;
        }
        dbg.size = meta[2];
        dbg.dmem_off = dmem - PMU_DBG_META_SIZE - meta[2];
        dbg.rd = 0;
        dbg.valid = 1;
        klog("ga10b: PMU dmesg ring dmem=%x off=%x size=%x meta rd=%x wr=%x "
             "wr_reg=%x\n", dmem, dbg.dmem_off, dbg.size, meta[0], meta[1], wr);
    }
    if (wr >= dbg.size) {
        klog("ga10b: PMU dmesg bad write offset %x >= %x\n", wr, dbg.size);
        return;
    }
    if (wr == dbg.rd)
        return;
    pmu_dmem_read(dbg.dmem_off, ring, dbg.size / 4u);
    const uint8_t* b = (const uint8_t*)ring;
    char line[192];
    uint32_t n = 0, it = dbg.rd, done = dbg.rd;
    while (it != wr) {
        uint8_t c = b[it];
        it = (it + 1u) % dbg.size;
        if (c == 0u) {
            line[n] = 0;
            klog("ga10b: PMU> %s\n", line);
            n = 0;
            done = it;
        } else if (n < sizeof(line) - 1u) {
            line[n++] = (c >= 0x20u && c < 0x7fu) ? (char)c : '.';
        }
    }
    if (n != 0u && !commit) {
        line[n] = 0;
        klog("ga10b: PMU> %s (unterminated)\n", line);
    }
    if (commit) {
        dbg.rd = done;
        ga10b_wr(PMU_CMD_TAIL(PMU_DBG_QUEUE), dbg.rd);
    }
}

/* polled stand-in for gk20a_pmu_isr + gk20a_pmu_handle_interrupts: ack what
 * the RTOS raised towards the host and service the dmesg ring on swgen1.
 * swgen0 (message queue) is consumed by the callers' head/tail polling. */
static void pmu_irq_service(void) {
    if (!ga10b.hw || (ga10b.skip & GA10B_SKIP_PMU_IRQ) != 0u)
        return;
    /* ga10b_pmu_is_interrupted: the IRQ registers are unreadable (bus poison)
     * while the RTOS holds priv lockdown, e.g. during BOOTSTRAP_GR_FALCONS */
    if ((ga10b_falcon_hwcfg2(&pmu_flcn) & PMU_PRIV_LOCKDOWN_BIT) != 0u)
        return;
    uint32_t stat = ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_IRQSTAT);
    if (stat == 0u || (stat & 0xffff0000u) == 0xbadf0000u)
        return;
    uint32_t mask = ga10b_rd(GA10B_PMU_PRISCV_BASE + GA10B_PRISCV_IRQMASK) &
                    ga10b_rd(GA10B_PMU_PRISCV_BASE + GA10B_PRISCV_IRQDEST);
    uint32_t intr = stat & mask;
    if (stat != dbg.last_stat) {
        klog("ga10b: PMU irq stat=%08x mask=%08x\n", stat, mask);
        dbg.last_stat = stat;
    }
    ga10b_wr(GA10B_PMU_FALCON_BASE + GA10B_FALCON_IRQSCLR,
             intr != 0u ? intr : stat);
    if ((intr & PMU_IRQ_SWGEN1) != 0u &&
        (ga10b.skip & GA10B_SKIP_PMU_DMESG) == 0u)
        pmu_dmesg_drain(1);
}

/* PMU VM + instance block + super surface.  The PMU RTOS accesses the super
 * surface through its own VM (cmdline super_surface descriptor is virtual,
 * DMAIDX_VIRT); the inst block physical address is handed to the PMU via
 * cmdline ctx_bind_addr (the LS RISC-V path self-binds, host does NOT write
 * pwr_pmu_new_instblk 0x10a480).  RAMIN pdb encoding matches the FIFO/GR
 * channels but with the sys_coh target (Orin IO-coherent): coh|vol|ver2|big64k. */
static int pmu_vm_init(void) {
    if (ga10b_gmmu_init(&st.mm) != 0)
        return -1;
    /* L4T same-board page-table walk (rdram.ko): PMU PDB target=ncoh(3),
     * PDE3/PDE2/PDE1 ap=ncoh(6), PTE ap=ncoh(6) even though Orin is
     * IO-coherent and the inst-block BIND is sys_coh (new_instblk=0x60...).
     * The page tables themselves use the non-coherent aperture. */
    st.mm.aperture = GA10B_GMMU_PTE_APERTURE_NCOH;
    /* L4T PMU VM (low_hole=0x10000, 512MB) allocates the super surface
     * top-down at va=0x1FE10000, trace at 0x1FFD4000.  Match that high VA
     * placement instead of the shared 0x10000000 bump base. */
    st.mm.va_next = 0x1FE10000ULL;
    if (ga10b_dma_alloc(4096, 4096, &st.inst_va, &st.inst_phy) != 0)
        return -1;
    uint32_t* w = st.inst_va;
    memset(w, 0, 4096);
    uint32_t pdb_lo = ga10b_ramin_pdb_lo_ap(st.mm.pdb_phy, 0x3u);  /* sys_ncoh */
    w[128] = pdb_lo;                          /* page_dir_base_lo */
    w[129] = (uint32_t)(st.mm.pdb_phy >> 32);
    /* L4T inst block also sets subctx0 valid + subctx0 PDB (w166/168/169),
     * pointing at the same PDB. */
    w[166] = 1u;
    w[168] = pdb_lo;
    w[169] = (uint32_t)(st.mm.pdb_phy >> 32);

    if (ga10b_dma_alloc(PMU_SS_SIZE, 4096, &st.ss_va, &st.ss_phy) != 0)
        return -1;
    memset(st.ss_va, 0, PMU_SS_SIZE);
    st.ss_gpu_va = ga10b_gmmu_map(&st.mm, st.ss_phy, PMU_SS_SIZE);
    if (st.ss_gpu_va == 0)
        return -1;
    /* RTOS trace buffer: nvgpu always provides one (cmdline trace_buf); the
     * RTOS writes debug traces here from early boot. */
    if (ga10b_dma_alloc(PMU_TRACE_SIZE, 4096, &st.trace_va, &st.trace_phy) != 0)
        return -1;
    memset(st.trace_va, 0, PMU_TRACE_SIZE);
    st.trace_gpu_va = ga10b_gmmu_map(&st.mm, st.trace_phy, PMU_TRACE_SIZE);
    if (st.trace_gpu_va == 0)
        return -1;
    /* hdr.address = super surface gpu_va (PMU ucode reads it back) */
    uint32_t* hdr = (uint32_t*)((uint8_t*)st.ss_va + PMU_SS_HDR_OFF);
    hdr[0] = (uint32_t)st.ss_gpu_va;
    hdr[1] = (uint32_t)(st.ss_gpu_va >> 32);
    __asm__ volatile("dsb sy" ::: "memory");
    klog("ga10b: PMU vm pdb=%llx inst=%llx ss=%llx gpu_va=%llx\n",
         (unsigned long long)st.mm.pdb_phy,
         (unsigned long long)st.inst_phy,
         (unsigned long long)st.ss_phy,
         (unsigned long long)st.ss_gpu_va);
    return 0;
}

int ga10b_pmu_init(const char* fw_dir) {
    if (st.ready)
        return 0;

    if (ga10b_fw_load(fw_dir, GA10B_FW_PMU_DESC, &st.desc) != 0 ||
        ga10b_fw_load(fw_dir, GA10B_FW_PMU_IMAGE, &st.image) != 0 ||
        ga10b_fw_load(fw_dir, GA10B_FW_PMU_SIG, &st.sig) != 0)
        goto fail;
    if (ga10b_fw_parse_pmu_desc(st.desc.va, st.desc.size,
                                st.image.size, &st.d) != 0) {
        klog("ga10b: pmu desc parse failed\n");
        goto fail;
    }
    if (ga10b_fw_check_pkc_sig(st.sig.va, st.sig.size,
                               GA10B_FALCON_PMU_NEXT_CORE) != 0) {
        klog("ga10b: pmu pkc sig sanity failed\n");
        goto fail;
    }
    if (!st.d.is_monitor_enabled) {
        klog("ga10b: pmu monitor disabled; BROM boot needs monitor regions\n");
        goto fail;
    }

    if (ga10b_dma_alloc((uint32_t)sizeof(pmu_boot_params_t), 256,
                        &st.args_va, &st.args_phy) != 0 ||
        ga10b_dma_alloc(PMU_CORE_DUMP_SIZE, 4096,
                        &st.dump_va, &st.dump_phy) != 0)
        goto fail;
    if (pmu_vm_init() != 0)
        goto fail;

    klog("ga10b: PMU ready: image=%u@%llx monitor code=%u@%x data=%u@%x "
         "manifest=%u@%x\n",
         st.image.size, (unsigned long long)st.image.phy,
         st.d.monitor_code_size, st.d.monitor_code_offset,
         st.d.monitor_data_size, st.d.monitor_data_offset,
         st.d.manifest_size, st.d.manifest_offset);
    st.ready = 1;
    return 0;

fail:
    ga10b_fw_free(&st.desc);
    ga10b_fw_free(&st.image);
    ga10b_fw_free(&st.sig);
    if (st.args_va) { ga10b_dma_free(st.args_va); st.args_va = NULL; }
    if (st.dump_va) { ga10b_dma_free(st.dump_va); st.dump_va = NULL; }
    return -1;
}

int ga10b_pmu_boot(void) {
    if (!st.ready)
        return -1;
    /* ACR-managed LS-PMU secure boot (nvgpu_pmu_rtos_init secure NEXT_CORE
     * path): ACR HS already authenticated the PMU next-core image into WPR1
     * and configured the BROM DMA addresses to point into WPR, so the driver
     * does NOT arm BROM itself.  It applies the PMU load-enable prods,
     * publishes the RTOS boot args via the mailboxes, starts the core and
     * waits for BROM pass + priv lockdown release. */
    if (ga10b.hw &&
        (ga10b_falcon_hwcfg2(&pmu_flcn) & PMU_PRIV_LOCKDOWN_BIT) == 0) {
        klog("ga10b: PMU RTOS already running (priv lockdown released)\n");
        return 0;
    }

    /* SLCG/BLCG PMU load-enable prods (ga10b_gating_reglist.c, prod=true) */
    ga10b_wr(0x0010a134u, 0x00040140u);
    ga10b_wr(0x0010aa74u, 0x00000400u);
    ga10b_wr(0x0010ae74u, 0x00000000u);
    ga10b_wr(0x0010aa70u, 0x00000045u);

    /* RTOS boot args (nvgpu_pmu_next_core_rtos_args_setup) */
    pmu_boot_params_t bp;
    memset(&bp, 0, sizeof(bp));
    bp.boot_params.bl.boot_type = PMU_BOOT_TYPE_RM;
    bp.boot_params.bl.size = (uint16_t)sizeof(pmu_boot_params_t);
    bp.boot_params.bl.version = PMU_BOOTLDR_VERSION;
    bp.boot_params.rtos.core_dump_phys = st.dump_phy;
    bp.boot_params.rtos.core_dump_size = PMU_CORE_DUMP_SIZE;
    bp.cmd_line_args.cpu_freq_hz = PMU_PWRCLK_HZ;
    bp.cmd_line_args.trace_buf.address.lo = (uint32_t)st.trace_gpu_va;
    bp.cmd_line_args.trace_buf.address.hi = (uint32_t)(st.trace_gpu_va >> 32);
    bp.cmd_line_args.trace_buf.params =
        (PMU_TRACE_SIZE & 0xffffffu) | (1u << 24);   /* size | DMAIDX_VIRT */
    /* super surface descriptor (virtual, DMAIDX_VIRT) + inst-block bind addr.
     * ctx_bind_addr = inst_phys>>12 | valid(bit30) | sys_coh(bits29:28=2),
     * ga10b_pmu_get_inst_block_config (NVGPU_USE_COHERENT_SYSMEM); PMU
     * self-binds its VM from this.  L4T capture: new_instblk=0x60107F27. */
    bp.cmd_line_args.super_surface.address.lo = (uint32_t)st.ss_gpu_va;
    bp.cmd_line_args.super_surface.address.hi = (uint32_t)(st.ss_gpu_va >> 32);
    bp.cmd_line_args.super_surface.params = PMU_SS_PARAMS;
    bp.cmd_line_args.ctx_bind_addr =
        (uint32_t)((st.inst_phy >> 12) & 0x0fffffffu) | 0x60000000u;
    memcpy(st.args_va, &bp, sizeof(bp));
    /* drain the NOCACHE args writes to DRAM before the monitor fetches them
     * via the EXTMEM2 DMA aperture (mailbox points at args_phy). */
    syscall3(SYS_DMA_SYNC, (ewokos_addr_t)st.args_va, sizeof(bp), 0);
    __asm__ volatile("dsb sy" ::: "memory");

    /* mailboxes carry the boot args through the next-core EXTMEM2 aperture */
    uint64_t mbox = st.args_phy + PMU_AMAP_EXTMEM2_START;
    ga10b_falcon_mbox_wr(&pmu_flcn, 0, (uint32_t)mbox);
    ga10b_falcon_mbox_wr(&pmu_flcn, 1, (uint32_t)(mbox >> 32));
    klog("ga10b: PMU secure boot args=%llx (BROM armed by ACR into WPR1)\n",
         (unsigned long long)mbox);

    /* startcpu + wait BROM pass (retcode==3); BROM fetches monitor from WPR */
    if (ga10b_priscv_boot(&pmu_flcn, 0, 2000) != 0)
        return -1;

    /* wait priv lockdown release (hwcfg2 bit13 -> 0) */
    if (ga10b_poll(GA10B_PMU_FALCON_BASE + GA10B_FALCON_HWCFG2,
                   PMU_PRIV_LOCKDOWN_BIT, 0, 5000) != 0) {
        klog("ga10b: PMU priv lockdown release timeout hwcfg2=%08x\n",
             ga10b_falcon_hwcfg2(&pmu_flcn));
        return -1;
    }
    klog("ga10b: PMU RTOS boot OK (BROM pass, priv lockdown released)\n");
    /* ga10b_pmu_enable_irq: re-enable the PMU MC subtree (stage_init masked
     * every FUNC top vector).  PMU vector=0x98 -> subtree2 -> top bit2,
     * leaf4 bit24 (subtree_mask=BIT(24)), leaf5=0. */
    ga10b_wr(GA10B_FUNC_BASE + GA10B_FUNC_INTR_TOP_EN_SET(0), 0x4u);
    ga10b_wr(GA10B_FUNC_BASE + GA10B_FUNC_INTR_LEAF_EN_SET(4), 0x01000000u);
    ga10b_wr(GA10B_FUNC_BASE + GA10B_FUNC_INTR_LEAF_EN_SET(5), 0x0u);
    return 0;
}

/* ---- FBQ message-queue RPC: PMU bootstraps FECS/GPCCS, writing their HS
 * signatures (the step the old lazy=0 blob skipped, root cause of 0x98).
 * Transport: super surface CMD queue LPQ (host->PMU) + MSG queue (PMU->host),
 * head/tail registers in hw_pwr.  No PMU IRQ service: poll the MSG queue. ---- */

/* wait for the PMU INIT message on the MSG queue and parse the v5 payload */
int ga10b_pmu_ipc_init(void) {
    if (st.ipc_ready)
        return 0;
    if (!ga10b.hw) { st.ipc_ready = 1; return 0; }
    if (st.ss_va == NULL)
        return -1;
    uint32_t head = 0, tail = 0;
    int got = 0;
    for (uint32_t i = 0; i < 2000u; i++) {
        pmu_irq_service();
        head = ga10b_rd(PMU_MSG_HEAD);
        tail = ga10b_rd(PMU_MSG_TAIL);
        if (head != tail) { got = 1; break; }
        ga10b_delay_us(1000);
    }
    if (!got) {
        klog("ga10b: PMU INIT message timeout (msg head=%u tail=%u)\n",
             head, tail);
        pmu_dmesg_drain(0);
        return -1;
    }
    /* element: msgq_hdr[0..3] pmu_hdr[4..7] cmdmgmt_init_v5[8..] */
    const uint8_t* e = (const uint8_t*)st.ss_va + PMU_SS_MSG_Q_OFF +
                       tail * PMU_MSG_ELEM_SIZE;
    if ((e[4] != PMU_UNIT_CMDMGMT && e[4] != PMU_UNIT_INIT_DGPU) ||
        e[9] != 0u) {
        klog("ga10b: PMU INIT msg bad unit=%u func=%u\n", e[4], e[9]);
        return -1;
    }
    st.lpq_phy_idx = e[22];                 /* queue_phy_id[1] = LPQ */
    st.sw_off  = (uint32_t)e[36] | ((uint32_t)e[37] << 8);
    st.sw_size = (uint32_t)e[38] | ((uint32_t)e[39] << 8);
    /* bitmap allocator bumps base 0 -> blk_size(32); heap_offset 0 is rejected
     * by pmu_fbq_cmd_setup, so the first usable offset is 32 when sw_off=0. */
    st.heap_next = (st.sw_off + 31u) & ~31u;
    if (st.heap_next == 0)
        st.heap_next = 32u;
    klog("ga10b: INIT raw[0..15]=%02x %02x %02x %02x %02x %02x %02x %02x "
         "%02x %02x %02x %02x %02x %02x %02x %02x\n",
         e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7],
         e[8], e[9], e[10], e[11], e[12], e[13], e[14], e[15]);
    klog("ga10b: INIT raw[16..39]=%02x %02x %02x %02x %02x %02x %02x %02x "
         "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
         "%02x %02x %02x %02x\n",
         e[16], e[17], e[18], e[19], e[20], e[21], e[22], e[23],
         e[24], e[25], e[26], e[27], e[28], e[29], e[30], e[31],
         e[32], e[33], e[34], e[35], e[36], e[37], e[38], e[39]);
    ga10b_wr(PMU_MSG_TAIL, (tail + 1u) % PMU_QUEUE_ELEMENTS);
    st.ipc_ready = 1;
    /* let RTOS process the INIT ack before first RPC */
    for (uint32_t i = 0; i < 5u; i++) {
        ga10b_delay_us(1000);
        pmu_irq_service();
    }
    klog("ga10b: PMU INIT ok lpq_phy=%u sw_managed=%u+%u (fw ready)\n",
         st.lpq_phy_idx, st.sw_off, st.sw_size);
    {
        const uint32_t* ssmd = (const uint32_t*)st.ss_va;
        for (int i = 0; i < 6; i++)
            klog("ga10b: ssmd[%d] id=%08x off=%x size=%x\n",
                 i, ssmd[i * 4], ssmd[i * 4 + 1], ssmd[i * 4 + 2]);
        const uint32_t* hdr = (const uint32_t*)((uint8_t*)st.ss_va +
                                               PMU_SS_HDR_OFF);
        klog("ga10b: ss hdr addr=%08x_%08x member_mask=%08x\n",
             hdr[1], hdr[0], hdr[2]);
    }
    return 0;
}

/* one ACR RPC through LPQ; PMU writes the result back into the CMD element
 * at offset 20 (pmu_payload_extract reads it there, not from the MSG msg). */
static int pmu_rpc(uint8_t function, const void* rpc, uint32_t size_rpc) {
    if (!st.ipc_ready)
        return -1;
    uint32_t heap_size = (size_rpc + 8u + 12u + 3u) & ~3u;
    uint32_t heap_off = st.heap_next;
    uint8_t seq = ++st.seq_id;

    uint8_t work[PMU_CMD_ELEM_SIZE];
    memset(work, 0, sizeof(work));
    uint32_t position = ga10b_rd(PMU_CMD_HEAD(st.lpq_phy_idx)) %
                        PMU_QUEUE_ELEMENTS;
    work[0] = (uint8_t)position;            /* fbq_hdr.element_index */
    work[4] = (uint8_t)(heap_size & 0xffu);
    work[5] = (uint8_t)(heap_size >> 8);
    work[6] = (uint8_t)(heap_off & 0xffu);
    work[7] = (uint8_t)(heap_off >> 8);
    work[8] = PMU_UNIT_ACR;                 /* pmu_hdr */
    work[9] = 12u;
    work[10] = PMU_CMD_CTRL_FLAGS;
    work[11] = seq;
    work[12] = PMU_RPC_CMD_ID;              /* nv_pmu_rpc_cmd */
    work[14] = (uint8_t)(size_rpc & 0xffu);
    work[15] = (uint8_t)(size_rpc >> 8);
    uint32_t dmem_ptr = heap_off + 20u;
    work[16] = (uint8_t)(dmem_ptr & 0xffu);
    work[17] = (uint8_t)((dmem_ptr >> 8) & 0xffu);
    work[18] = (uint8_t)((dmem_ptr >> 16) & 0xffu);
    work[19] = (uint8_t)((dmem_ptr >> 24) & 0xffu);
    memcpy(work + 20, rpc, size_rpc);       /* RPC body @20 */

    uint8_t* elem = (uint8_t*)st.ss_va + PMU_SS_CMD_Q_OFF +
                    PMU_LPQ_LOGICAL * 0x8000u + position * PMU_CMD_ELEM_SIZE;
    memcpy(elem, work, PMU_CMD_ELEM_SIZE);  /* push: fixed 2048B copy */
    /* publish CPU-written element to PoC before notifying PMU: the FBQ DMA
     * read is non-coherent, so clean+invalidate the kernel alias (op0). */
    syscall3(SYS_DMA_SYNC, (ewokos_addr_t)elem, PMU_CMD_ELEM_SIZE, 0);
    __asm__ volatile("dsb sy" ::: "memory");
    ga10b_wr(PMU_CMD_HEAD(st.lpq_phy_idx),
             (position + 1u) % PMU_QUEUE_ELEMENTS);

    /* drain MSG queue until the response with our seq_id arrives */
    int got = 0;
    for (uint32_t i = 0; i < 5000u; i++) {
        pmu_irq_service();
        uint32_t mhead = ga10b_rd(PMU_MSG_HEAD);
        uint32_t mtail = ga10b_rd(PMU_MSG_TAIL);
        if (mhead == mtail) { ga10b_delay_us(1000); continue; }
        const uint8_t* me = (const uint8_t*)st.ss_va + PMU_SS_MSG_Q_OFF +
                            mtail * PMU_MSG_ELEM_SIZE;
        uint8_t rseq = me[7];
        ga10b_wr(PMU_MSG_TAIL, (mtail + 1u) % PMU_QUEUE_ELEMENTS);
        /* response matched by pmu_hdr.seq_id (me[7]); me[4]=unit_id,
         * me[8]=rpc_header.unit_id (NOT the 0x80 cmd_type). */
        if (rseq == seq) { got = 1; break; }
    }
    st.heap_next = (st.sw_off + 31u) & ~31u;   /* reuse heap (single RPC) */
    if (st.heap_next == 0)
        st.heap_next = 32u;
    if (!got) {
        klog("ga10b: PMU RPC func=%u response timeout cmd_head=%u cmd_tail=%u "
             "msg_head=%u msg_tail=%u pos=%u heap_off=%u heap_sz=%u\n",
             function, ga10b_rd(PMU_CMD_HEAD(st.lpq_phy_idx)),
             ga10b_rd(PMU_CMD_TAIL(st.lpq_phy_idx)),
             ga10b_rd(PMU_MSG_HEAD), ga10b_rd(PMU_MSG_TAIL),
             position, heap_off, heap_size);
        pmu_dmesg_drain(0);
        klog("ga10b: RPC elem[0..19]=%02x %02x %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
             elem[0], elem[1], elem[2], elem[3], elem[4], elem[5], elem[6],
             elem[7], elem[8], elem[9], elem[10], elem[11], elem[12], elem[13],
             elem[14], elem[15], elem[16], elem[17], elem[18], elem[19]);
        klog("ga10b: PMU state cpuctl=%08x os=%08x irqstat=%08x irqmask=%08x "
             "irqdest=%08x priscv_irqmask=%08x hwcfg2=%08x\n",
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_CPUCTL),
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_OS),
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_IRQSTAT),
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_IRQMASK),
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_IRQDEST),
             ga10b_rd(GA10B_PMU_PRISCV_BASE + GA10B_PRISCV_IRQMASK),
             ga10b_falcon_hwcfg2(&pmu_flcn));
        klog("ga10b: PMU riscv cpuctl=%08x irqdest=%08x irqmask=%08x "
             "br_retc=%08x | q0 head=%08x tail=%08x q1 head=%08x tail=%08x\n",
             ga10b_rd(GA10B_PMU_PRISCV_BASE + GA10B_PRISCV_CPUCTL),
             ga10b_rd(GA10B_PMU_PRISCV_BASE + GA10B_PRISCV_IRQDEST),
             ga10b_rd(GA10B_PMU_PRISCV_BASE + GA10B_PRISCV_IRQMASK),
             ga10b_rd(GA10B_PMU_PRISCV_BASE + GA10B_PRISCV_BR_RETCODE),
             ga10b_rd(PMU_CMD_HEAD(0)), ga10b_rd(PMU_CMD_TAIL(0)),
             ga10b_rd(PMU_CMD_HEAD(1)), ga10b_rd(PMU_CMD_TAIL(1)));
        klog("ga10b: PMU riscv irqsset=%08x irqsclr=%08x | falcon irqsset=%08x "
             "itfen=%08x new_instblk=%08x\n",
             ga10b_rd(GA10B_PMU_PRISCV_BASE + GA10B_PRISCV_IRQSSET),
             ga10b_rd(GA10B_PMU_PRISCV_BASE + GA10B_PRISCV_IRQSCLR),
             ga10b_rd(GA10B_PMU_FALCON_BASE + 0x000u),
             ga10b_rd(GA10B_PMU_FALCON_BASE + 0x048u),
             ga10b_rd(0x0010a480u));
        klog("ga10b: PMU riscv irqblk 500=%08x 504=%08x 508=%08x 50c=%08x "
             "510=%08x 514=%08x 518=%08x 51c=%08x\n",
             ga10b_rd(0x0010b500u), ga10b_rd(0x0010b504u),
             ga10b_rd(0x0010b508u), ga10b_rd(0x0010b50cu),
             ga10b_rd(0x0010b510u), ga10b_rd(0x0010b514u),
             ga10b_rd(0x0010b518u), ga10b_rd(0x0010b51cu));
        if (st.dump_va) {
            const uint32_t* cd = (const uint32_t*)st.dump_va;
            klog("ga10b: PMU coredump[0..7]=%08x %08x %08x %08x %08x %08x "
                 "%08x %08x\n", cd[0], cd[1], cd[2], cd[3], cd[4], cd[5],
                 cd[6], cd[7]);
        }
        if (st.trace_va) {
            syscall3(SYS_DMA_SYNC, (ewokos_addr_t)st.trace_va, PMU_TRACE_SIZE, 1);
            const uint32_t* tr = (const uint32_t*)st.trace_va;
            klog("ga10b: PMU trace[0..15]=%08x %08x %08x %08x %08x %08x %08x "
                 "%08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 tr[0], tr[1], tr[2], tr[3], tr[4], tr[5], tr[6], tr[7],
                 tr[8], tr[9], tr[10], tr[11], tr[12], tr[13], tr[14], tr[15]);
        }
        /* falcon wrapper debug state: where is the RTOS stuck? */
        klog("ga10b: PMU flcn os=%08x dbg1=%08x dbginfo=%08x imstat=%08x "
             "traceidx=%08x tracepc=%08x exterr=%08x/%08x bootvec=%08x\n",
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_OS),
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_DEBUG1),
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_DEBUGINFO),
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_IMSTAT),
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_TRACEIDX),
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_TRACEPC),
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_EXTERRSTAT),
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_EXTERRADDR),
             ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_BOOTVEC));
        /* re-read super surface ssmd + hdr after the 10s wait: did the RTOS
         * ever fill the member descriptors (FBQ setup prerequisite)? */
        {
            syscall3(SYS_DMA_SYNC, (ewokos_addr_t)st.ss_va, PMU_SS_SIZE, 1);
            const uint32_t* ssmd = (const uint32_t*)st.ss_va;
            klog("ga10b: ssmd late[0..3] id=%08x off=%x sz=%x | [1] id=%08x "
                 "off=%x sz=%x | [2] id=%08x off=%x sz=%x | [3] id=%08x off=%x "
                 "sz=%x\n",
                 ssmd[0], ssmd[1], ssmd[2], ssmd[4], ssmd[5], ssmd[6],
                 ssmd[8], ssmd[9], ssmd[10], ssmd[12], ssmd[13], ssmd[14]);
            const uint32_t* hdr = (const uint32_t*)((uint8_t*)st.ss_va +
                                                   PMU_SS_HDR_OFF);
            klog("ga10b: ss hdr late addr=%08x_%08x member_mask=%08x "
                 "dmem_max=%08x\n", hdr[1], hdr[0], hdr[2], hdr[3]);
        }
        return -1;
    }
    uint8_t status = elem[20 + 3];          /* rpc_header.flcn_status */
    if (status != 0) {
        klog("ga10b: PMU RPC func=%u FAILED flcn_status=%u\n",
             function, status);
        return -1;
    }
    klog("ga10b: PMU RPC func=%u OK (flcn_status=0)\n", function);
    return 0;
}

/* FECS HS signature register reads 0xaf88 once the PMU has bootstrapped it */
int ga10b_pmu_gr_done(void) {
    if (!ga10b.hw || !ga10b.mapped)
        return 0;
    return ga10b_rd(PMU_FECS_SIG_REG) == PMU_FECS_SIG_VAL;
}

/* INIT_WPR_REGION then BOOTSTRAP_GR_FALCONS (lsfm_sw_gv100.c): the PMU
 * bootstraps FECS+GPCCS (mask 0xC) and writes their HS signatures. */
int ga10b_pmu_bootstrap_gr(void) {
    if (st.gr_loaded)
        return 0;
    if (!ga10b.hw) {
        st.gr_loaded = 1;
        klog("ga10b: [dry] PMU bootstrap FECS/GPCCS skipped\n");
        return 0;
    }
    if (ga10b_pmu_gr_done()) {
        klog("ga10b: FECS/GPCCS already bootstrapped (HS signatures present)\n");
        st.gr_loaded = 1;
        return 0;
    }
    if (ga10b_pmu_ipc_init() != 0)
        return -1;

    uint8_t wpr[20];                        /* INIT_WPR_REGION, 20B sent */
    memset(wpr, 0, sizeof(wpr));
    wpr[0] = PMU_UNIT_ACR;
    wpr[1] = PMU_RPC_INIT_WPR;
    wpr[12] = 1u;                           /* wpr_regionId = WPR1 */
    if (pmu_rpc(PMU_RPC_INIT_WPR, wpr, sizeof(wpr)) != 0)
        return -1;
    ga10b_delay_us(2000);                   /* LSPMU priv-lockdown window */

    uint8_t bs[32];                         /* BOOTSTRAP_GR_FALCONS, 32B sent */
    memset(bs, 0, sizeof(bs));
    bs[0] = PMU_UNIT_ACR;
    bs[1] = PMU_RPC_BOOTSTRAP_GR;
    bs[12] = 0x0cu;                         /* FECS(2)|GPCCS(3), flags=0 RESET_YES */
    if (pmu_rpc(PMU_RPC_BOOTSTRAP_GR, bs, sizeof(bs)) != 0)
        return -1;
    ga10b_delay_us(5000);

    st.gr_loaded = 1;
    klog("ga10b: FECS/GPCCS bootstrapped by PMU (HS signatures written)\n");
    return 0;
}