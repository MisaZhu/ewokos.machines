/*
 * v3d_g2d.c - VideoCore VII (V3D) hardware back end for the EwokOS
 * raspberry-pi5 bsp_g2d layer, ported from the bare-metal g2d library
 * (g2d_v3d.c, proven on real Pi 5 hardware: scopy-pattern TMU writes and
 * the legal V3D 7.1 last-THRSW/thread-end sequence on every exit path).
 *
 * Differences from the bare-metal version:
 *   - registers are accessed through SYS_MEM_MAP-mapped windows instead
 *     of physical addresses (V3D is outside the EwokOS MMIO window, so
 *     machines/raspi5/kernel/bsp/hw_info_arch.c whitelists it);
 *   - CSD code/uniform/scratch staging is dma_alloc'ed (physically
 *     contiguous sys_dma memory), and a V3D-local page table maps only
 *     the RAM ranges which the QPU is allowed to access;
 *   - canvases arrive as virtual addresses with their physical bases
 *     supplied by the caller (bsp_g2d *_phy); cache maintenance is
 *     skipped for NOCACHE dma canvases;
 *   - zero copy: canvas pixels are never copied - the kernels operate
 *     directly on the caller's buffers through their V3D IOVAs; the three
 *     kernels are preloaded into dma staging at init and a dispatch only
 *     refreshes the small uniform block.
 *
 * Register map follows the Linux drm/v3d driver (v3d_regs.h, V3D 7.x):
 *   hub:  0x1002000000   V3D_HUB_IDENT0/1/2 (0x08/0x0c/0x10), MMU regs
 *   core0:0x1002008000   V3D_CTL_IDENT0 (0x00), L2C (0x20/0x30),
 *                        INT_STS/CLR (0x50/0x58), CSD_QUEUED_CFG0..6
 *                        (0x930..), CSD_STATUS (0x900)
 *   sms:  0x1002030800   power/state machine
 *   pm:   0x107D200000   GRAFX power domain (V3D reset)
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <ewoksys/thread.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/sys.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <ewoksys/interrupt.h>
#include <ewoksys/kernel_tic.h>
#include <arch/bcm2712/mailbox.h>
#include "v3d_g2d.h"
#include "g2d_qpu_kernels.h"

/* ---- physical bases (whitelisted by check_mem_map_arch) ---- */
#define V3D_PHY_BASE   0x1002000000ULL
#define V3D_MAP_SIZE   0x40000u      /* hub + core0 + SMS */
#define PM_PHY_BASE    0x107D200000ULL
#define PM_MAP_SIZE    0x1000u

/* ---- offsets inside the mapped V3D window ---- */
#define V3D_HUB_OFF    0x00000u
#define V3D_CORE0_OFF  0x08000u
#define V3D_SMS_OFF    0x30800u

/* ---- hub registers ---- */
#define HUB_IDENT0 0x08u
#define HUB_IDENT3 0x14u
#define HUB_INT_STS 0x50u
#define HUB_INT_CLR 0x58u
#define HUB_INT_MMU_CAP (1u << 3)
#define HUB_INT_MMU_PTI (1u << 4)
#define HUB_INT_MMU_WRV (1u << 5)

/* V3D 3.x+ GPU-local single-level MMU.  The upstream V3D driver always
 * enables it before submitting jobs; leaving the reset-time bypass state
 * active gives CSD/TMU traffic no address isolation and was observed to
 * corrupt low RAM on BCM2712 D0.  V3D has a 4GB, 4KB-page IOVA space,
 * hence a full table is 4MB. */
#define V3D_MMUC_CONTROL          0x1000u
#define V3D_MMUC_ENABLE           (1u << 0)
#define V3D_MMUC_FLUSH            (1u << 1)
#define V3D_MMUC_FLUSHING         (1u << 2)
#define V3D_MMU_CTL               0x1200u
#define V3D_MMU_PT_PA_BASE        0x1204u
#define V3D_MMU_HIT               0x1208u
#define V3D_MMU_MISSES            0x120cu
#define V3D_MMU_STALLS            0x1210u
#define V3D_MMU_VIO_ID            0x122cu
#define V3D_MMU_VIO_ID_MASK       0x7fu
#define V3D_MMU_ILLEGAL_ADDR      0x1230u
#define V3D_MMU_VIO_ADDR          0x1234u
#define V3D_MMU_DEBUG_INFO        0x1238u
#define V3D_MMU_ENABLE            (1u << 0)
#define V3D_MMU_TLB_CLEAR         (1u << 2)
#define V3D_MMU_TLB_CLEARING      (1u << 7)
#define V3D_MMU_WRITE_INT         (1u << 10)
#define V3D_MMU_WRITE_ABORT       (1u << 11)
#define V3D_MMU_WRITE_FAULT       (1u << 12)
#define V3D_MMU_PT_INVALID_ENABLE (1u << 16)
#define V3D_MMU_PT_INVALID_INT    (1u << 18)
#define V3D_MMU_PT_INVALID_ABORT  (1u << 19)
#define V3D_MMU_PT_INVALID_FAULT  (1u << 20)
#define V3D_MMU_CAP_INT           (1u << 25)
#define V3D_MMU_CAP_ABORT         (1u << 26)
#define V3D_MMU_CAP_FAULT         (1u << 27)
#define V3D_MMU_ILLEGAL_ENABLE    (1u << 31)
#define V3D_MMU_PTE_VALID         (1u << 28)
#define V3D_MMU_PTE_WRITEABLE     (1u << 29)
#define V3D_MMU_PFN_LIMIT         (1u << 24) /* PTE AXI address is 36-bit */
#define V3D_MMU_PAGES             (1u << 20) /* 4GB / 4KB */
#define V3D_MMU_PT_BYTES          (V3D_MMU_PAGES * sizeof(uint32_t))
#define V3D_MMU_ADDR_LIMIT        (1ull << 32)
/* ---- core registers ---- */
#define CTL_L2CACTL  0x20u
#define CTL_L2TCACTL 0x30u
#define CTL_IDENT1   0x04u

/* CSD dispatch (core0 window) */
#define CSD_QUEUED_CFG0 0x930u
#define CSD_STATUS      0x900u
#define CSD_CURRENT_CFG5 0x96cu
#define CSD_CURRENT_CFG6 0x970u
#define CSD_CURRENT_CFG7 0x974u
#define INT_STS         0x50u
#define INT_CLR         0x58u
#define INT_MSK_STS     0x5cu
#define INT_MSK_SET     0x60u
#define INT_MSK_CLR     0x64u
#define INT_CSD_DONE    (1u << 6)

/* core0 interrupt line: bcm2712.dtsi v3d node lists <GIC_SPI 250> (hub)
 * and <GIC_SPI 249> (core0); the EwokOS user-space irq number is the GIC
 * INTID, i.e. 32 + SPI (machines/raspi5/kernel/bsp/irq.c passes raw
 * INTIDs through).  CSD_DONE is a core interrupt. */
#define V3D_CORE0_IRQ   (32u + 249u)

/* ---- PM power domain (reset GRAFX_V3D) ---- */
#define PM_GRAFX_OFF 0x10cu
#define PM_PASSWORD  0x5A000000u
#define PM_V3DRSTN   (1u << 6)

#define CSD_CODE_WORDS 512   /* 344-word argb_alpha (endpoint-exact blend) */
#define CSD_UNIF_WORDS 64
#define CSD_SCRATCH_BYTES (4096u * 4u)   /* TMU write scratch size */
#define CSD_POLL_SPIN_LIMIT 2000000u
#define CSD_POLL_YIELD_LIMIT 256u
/* IRQ wait: the worker parks on its own tid with this slice as a
 * lost-interrupt bound (tick-granular, so ~2 frames), re-checking the
 * done sequence and the status latch each wake, until the same ~256 ms
 * budget the yield loop has.  CSD_IRQ_MISS_MAX consecutive dispatches
 * that completed by the latch without a delivered interrupt switch the
 * driver back to polling for good (wrong INTID / routing: slow, never
 * stuck). */
#define CSD_IRQ_WAIT_SLICE_US 1500u
#define CSD_IRQ_WAIT_LIMIT_NS (256ull * 1000000ull)
#define CSD_IRQ_MISS_MAX 8u

/* Raspberry Pi firmware property tags and clock ID. */
#define FW_GET_CLOCK_RATE      0x00030002u
#define FW_GET_MAX_CLOCK_RATE  0x00030004u
#define FW_SET_CLOCK_RATE      0x00038002u
#define FW_CLOCK_V3D           5u
#define FW_RESPONSE            0x80000000u

typedef struct {
    uint32_t buf_size;
    uint32_t code;
    struct {
        uint32_t tag;
        uint32_t value_buf_size;
        uint32_t value_len;
        uint32_t clock_id;
        uint32_t rate_hz;
    } tag;
    uint32_t end_tag;
} __attribute__((packed)) g2d_clock_get_req_t;

typedef struct {
    uint32_t buf_size;
    uint32_t code;
    struct {
        uint32_t tag;
        uint32_t value_buf_size;
        uint32_t value_len;
        uint32_t clock_id;
        uint32_t rate_hz;
        uint32_t skip_turbo;
    } tag;
    uint32_t end_tag;
} __attribute__((packed)) g2d_clock_set_req_t;

/* BRING-UP SWITCH:
 *   G2D_HW_PROBE_ONLY
 *                   0 = full proven bring-up (probe -> staging -> sms ->
 *                       hub cfg -> l2c)
 *                   1 = probe + window maps + dma staging only; no V3D
 *                       register writes at all (_ok still set)
 *
 * History note: writing PM_GRAFX.V3DRSTN can hang the Device write /
 * dsb here (the GRAFX domain resets under the write), hence the
 * G2D_SKIP_PM_RESET switch below. The boot-hang seen while integrating
 * this driver was eventually tracked to kernel address-space switching,
 * not to these register writes (missing TLB invalidation in
 * __set_translation_table_base, fixed in
 * kernel/platform/aarch64/arch/v8/system.S). */
#define G2D_HW_PROBE_ONLY 0

/* DEBUG SWITCH: PM_GRAFX.V3DRSTN power-cycle.  The proven bare-metal
 * sequence REQUIRES it - without the power-cycle the QPU array never
 * launches (first CSD dispatch reports done-timeout, ic-miss stays 0).
 * An earlier EwokOS attempt hung the Device write / dsb inside this
 * block; that predates the kernel TLB-switch fix (see
 * kernel/platform/aarch64/arch/v8/system.S), so the write is enabled
 * again for the first real V3D experiments. */
#define G2D_SKIP_PM_RESET 0

/* L2TFLM mode bits (empirically settled on real Pi 5 hardware):
 *   0 = clean + invalidate (the ONLY mode usable for the pre-job walk)
 *   2 = clean: writes dirty lines back but LEAVES THEM RESIDENT -
 *       correct for the post-job flush, fatal before a job.
 *   DO NOT retry a cheaper pre-job walk with mode 2: it was tried and
 *       produced wrong pixels on the panel - the old lines survive the
 *       walk and the next job reads them. */

/* ---- mapped register windows ---- */
static volatile uint32_t *_v3d;   /* V3D block (hub at +0) */
static volatile uint32_t *_pm;    /* PM power domain */

static inline volatile uint32_t *v3d_hub(void)
{
    return _v3d + (V3D_HUB_OFF / 4);
}

static inline volatile uint32_t *v3d_core(void)
{
    return _v3d + (V3D_CORE0_OFF / 4);
}

/* ---- dma staging (physically contiguous, NOCACHE) ----
 * The ARGB kernels are PRELOADED once at init into their own
 * 256-word dma regions, so a dispatch never re-copies code - only the
 * small uniform block is refreshed per call.  The GPU operates directly
 * on the caller's canvas addresses (zero copy: the V3D IOVAs derived from
 * the caller-supplied physical bases are what the kernels write to). */
#define KERN_FILL 0
#define KERN_BLIT 1
#define KERN_ALPHA 2
#define KERN_ROTATE 3
#define KERN_SCALE_POW2 4
#define KERN_ROT90 5
#define KERN_COPY 6
#define KERN_FILL4 7
#define KERN_GAUSS_H5 8
#define KERN_GAUSS_V5 9
#define KERN_GAUSS_H9 10
#define KERN_GAUSS_V9 11
#define KERN_GAUSS_H3 12
#define KERN_GAUSS_V3 13
#define KERN_GAUSS_H7 14
#define KERN_GAUSS_V7 15
#define KERN_N 16
static uint64_t *_kcode[KERN_N];     /* per-kernel code staging VA (dma) */
static uint32_t _kcode_p[KERN_N];    /* per-kernel code staging physical */
static const uint64_t *_ksrc[KERN_N];/* kernel source arrays */
static unsigned _ksrc_n[KERN_N];     /* kernel instruction counts */
static uint32_t *_unif;            /* uniform staging (64 words) */
static uint32_t *_scratch;         /* TMU write scratch (16 KiB) */
static uint32_t _unif_p, _scratch_p;
static uint32_t *_mmu_pt;
static ewokos_addr_t _mmu_pt_p, _mmu_illegal_p;

static int _inited = 0;
static int _ok = 0;
static int _num_qpus = 1;
static int _trailing_l2t_quirk = 0;
static uint32_t _mmu_debug_info = 0;
static uint32_t _mmu_va_width = 32;

/* V3D clock rate in Hz as set by g2d_clock_set_max() at init (0 until
   confirmed, or when the property mailbox is unavailable) */
static uint32_t _v3d_clock_hz = 0;

/* Physical RAM ranges captured at init.  These are both the validation
 * contract for callers and the only ranges installed in the V3D MMU. */
static ewokos_addr_t _ram_dma_base = 0, _ram_dma_top = 0;
static ewokos_addr_t _ram_contig_base = 0, _ram_contig_top = 0;

/* CACHE-MAINTENANCE CONTRACT (EL0-portable):
 *
 * Every buffer handed to this driver that the QPU may read or write
 * must be mapped Non-Cacheable in each process.  The contig shm slab is
 * mapped PTE_ATTR_NOCACHE by the kernel for exactly this reason, and
 * gpu_phys() only accepts canvases flagged contig - so both directions
 * of CPU<->GPU visibility go straight through DRAM and NO maintenance
 * by virtual address is needed.
 *
 * DC CVAC/CIVAC executed from EL0 TRAP unless SCTLR_EL1.UCI is set, so
 * the emit-the-instruction implementations are compiled out.  Flip to 1
 * ONLY if the kernel later grants UCI AND canvases become cacheable
 * again. */
#define G2D_MAINT_BY_DC 0

static void g2d_dcache_clean(void *addr, size_t len)
{
#if G2D_MAINT_BY_DC
    uintptr_t p = (uintptr_t)addr & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)addr + len;

    for (; p < end; p += 64)
        __asm__ __volatile__("dc cvac, %0" :: "r"(p));
#endif
    (void)addr; (void)len;
    __asm__ __volatile__("dsb sy");
}

static void g2d_dcache_clean_invalidate(void *addr, size_t len)
{
#if G2D_MAINT_BY_DC
    uintptr_t p = (uintptr_t)addr & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)addr + len;

    for (; p < end; p += 64)
        __asm__ __volatile__("dc civac, %0" :: "r"(p));
#endif
    (void)addr; (void)len;
    __asm__ __volatile__("dsb sy");
}

static void g2d_dcache_invalidate(void *addr, size_t len)
{
#if G2D_MAINT_BY_DC
    uintptr_t p = (uintptr_t)addr & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)addr + len;

    for (; p < end; p += 64)
        __asm__ __volatile__("dc ivac, %0" :: "r"(p));
#endif
    (void)addr; (void)len;
    __asm__ __volatile__("dsb sy");
}

/* (delay helper) the driver runs as a user-space daemon, so register-settle
 * waits use the OS sleep API.  The bare-metal harness polled CNTPCT_EL0
 * directly, which traps at EL0 unless the kernel enables CNTKCTL_EL1
 * timer access - never assume that in OS-portable code.  For these small
 * values usleep() busy-spins on the fine counter for exactly the requested
 * microseconds (it no longer rounds up to a tick); that still meets the
 * settle lower bounds documented at the call sites. */
#define g2d_delay_us(us) usleep(us)

/* spin-wait hint for the register polls below: a tight loop of
 * device-memory reads issues a fresh uncached AXI transaction every
 * iteration and still occupies the ARM pipeline between them; yielding
 * lets a second thread on the same core make progress while the GPU
 * flushes, so the waits show up as mostly-idle instead of 100% busy.
 * The flush duration itself is GPU-bound - the hint only moves the
 * waste off the CPU. */
#define g2d_poll_hint() __asm__ __volatile__("yield")

/* cached sys_dma window: captured once at init.  is_dma_addr() runs
 * three times per dispatch, so re-querying SYS_GET_SYS_INFO every time
 * burned kernel transitions; the window never changes at runtime. */
static ewokos_addr_t _dma_v_base = 0, _dma_v_size = 0;

/* is the pointer inside the sys_dma NOCACHE window? (no cache
 * maintenance needed there) */
static int is_dma_addr(const void *v)
{
    uintptr_t a = (uintptr_t)v;

    return a >= (uintptr_t)_dma_v_base &&
           a < (uintptr_t)(_dma_v_base + _dma_v_size);
}

static int g2d_clock_get(uint32_t property_tag, uint32_t *rate_hz)
{
    g2d_clock_get_req_t *req;
    ewokos_addr_t vaddr;
    ewokos_addr_t phys;
    mail_message_t msg;
    int result = -1;

    if (rate_hz == NULL)
        return -1;
    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0)
        return -1;
    req = (g2d_clock_get_req_t *)(uintptr_t)vaddr;
    memset(req, 0, sizeof(*req));
    req->buf_size = sizeof(*req);
    req->tag.tag = property_tag;
    req->tag.value_buf_size = 8;
    req->tag.value_len = 4;
    req->tag.clock_id = FW_CLOCK_V3D;

    phys = dma_phy_addr(0, vaddr);
    if (phys != 0 && (phys >> 32) == 0) {
        memset(&msg, 0, sizeof(msg));
        msg.data = (((uint32_t)phys | MAILBOX_VC_ALIAS_NONCACHED) >> 4);
        msg.channel = PROPERTY_CHANNEL;
        if (bcm2712_mailbox_call_timeout(&msg, 0) == 0 &&
            (req->code & FW_RESPONSE) != 0 &&
            (req->tag.value_len & FW_RESPONSE) != 0 &&
            (req->tag.value_len & ~FW_RESPONSE) >= 4 &&
            req->tag.clock_id == FW_CLOCK_V3D && req->tag.rate_hz != 0) {
            *rate_hz = req->tag.rate_hz;
            result = 0;
        }
    }
    dma_free(0, vaddr);
    return result;
}

static int g2d_clock_set(uint32_t rate_hz)
{
    g2d_clock_set_req_t *req;
    ewokos_addr_t vaddr;
    ewokos_addr_t phys;
    mail_message_t msg;
    int result = -1;

    if (rate_hz == 0)
        return -1;
    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0)
        return -1;
    req = (g2d_clock_set_req_t *)(uintptr_t)vaddr;
    memset(req, 0, sizeof(*req));
    req->buf_size = sizeof(*req);
    req->tag.tag = FW_SET_CLOCK_RATE;
    req->tag.value_buf_size = 12;
    req->tag.value_len = 12;
    req->tag.clock_id = FW_CLOCK_V3D;
    req->tag.rate_hz = rate_hz;
    req->tag.skip_turbo = 0;

    phys = dma_phy_addr(0, vaddr);
    if (phys != 0 && (phys >> 32) == 0) {
        memset(&msg, 0, sizeof(msg));
        msg.data = (((uint32_t)phys | MAILBOX_VC_ALIAS_NONCACHED) >> 4);
        msg.channel = PROPERTY_CHANNEL;
        if (bcm2712_mailbox_call_timeout(&msg, 0) == 0 &&
            (req->code & FW_RESPONSE) != 0 &&
            (req->tag.value_len & FW_RESPONSE) != 0 &&
            (req->tag.value_len & ~FW_RESPONSE) >= 8 &&
            req->tag.clock_id == FW_CLOCK_V3D)
            result = 0;
    }
    dma_free(0, vaddr);
    return result;
}

static void g2d_clock_set_max(void)
{
    uint32_t max_hz;
    uint32_t actual_hz;

    if (bcm2712_mailbox_init() == 0 ||
        g2d_clock_get(FW_GET_MAX_CLOCK_RATE, &max_hz) != 0 ||
        g2d_clock_set(max_hz) != 0 ||
        g2d_clock_get(FW_GET_CLOCK_RATE, &actual_hz) != 0) {
        slog("g2d: V3D clock setup failed\r\n");
        return;
    }
    slog("g2d: V3D clock max=%u Hz actual=%u Hz\r\n", max_hz, actual_hz);
    _v3d_clock_hz = actual_hz;
}

/* ------------------------------------------------------------------ */
/* bring-up                                                            */
/* ------------------------------------------------------------------ */

/* Re-enable the V3D L2 cache (a reset may leave it disabled). */
static void g2d_l2c_enable(void)
{
    v3d_core()[CTL_L2CACTL / 4] = (1u << 2) | (1u << 0);    /* L2CCLR | L2CENA */
    __asm__ __volatile__("dsb sy");
}

/* Ranged post-job flush: drain the TMU write combiner, then clean
 * (mode 2, lines stay resident) the L2T over [lo, hi).  Bounded like
 * g2d_invalidate_range; the range registers are programmed explicitly
 * on every call. */
static void g2d_flush_l2_range(uint32_t lo, uint32_t hi)
{
    uint32_t i;

    v3d_core()[CTL_L2TCACTL / 4] = (1u << 8);               /* TMUWCF */
    for (i = 0; i < 2000000 && (v3d_core()[CTL_L2TCACTL / 4] & (1u << 8)); i++)
        g2d_poll_hint();
    v3d_core()[0x34 / 4] = lo & ~63u;                       /* L2TFLSTA */
    v3d_core()[0x38 / 4] = (hi + 63u) & ~63u;               /* L2TFLEND */
    v3d_core()[CTL_L2TCACTL / 4] = (1u << 0) | (2u << 1);   /* L2TFLS | CLEAN */
    for (i = 0; i < 2000000 && (v3d_core()[CTL_L2TCACTL / 4] & (1u << 0)); i++)
        g2d_poll_hint();
    __asm__ __volatile__("dsb sy");
}

/* Write back dirty V3D caches to DRAM: flush the TMU write combiner,
 * then the L2T in CLEAN mode over the WHOLE IOVA range.  This is the
 * fallback for a dispatch without usable maint ranges; callers that
 * know their destination extent use g2d_flush_l2_range instead.  The
 * range registers are reprogrammed explicitly on every walk (they are
 * stateful - a narrowed range left behind by an earlier ranged walk
 * would make this flush miss the canvas's dirty lines). */
static void g2d_flush_l2(void)
{
    g2d_flush_l2_range(0u, ~0u);
}

/* Ranged L2T clean+invalidate (mode 0) over [lo, hi), 64-byte aligned.
 * Same walk semantics as g2d_invalidate_caches but bounded by
 * L2TFLSTA/L2TFLEND, so the walk cost scales with the touched address
 * span instead of the whole 4 GB IOVA space.  The range registers are
 * STATEFUL: program them explicitly on every call and never rely on a
 * previous walk's settings (a narrowed range left behind once made a
 * batched op's final band skip its writeback). */
static void g2d_invalidate_range(uint32_t lo, uint32_t hi)
{
    uint32_t i;

    v3d_core()[0x34 / 4] = lo & ~63u;             /* L2TFLSTA */
    v3d_core()[0x38 / 4] = (hi + 63u) & ~63u;     /* L2TFLEND (hi clamps to ~0u) */
    /* mode 0 = clean + invalidate: the lines MUST be dropped here (see
     * the L2TFLM mode note above the switches) */
    v3d_core()[0x30 / 4] = (1u << 0) | (0u << 1);   /* L2TCACTL: L2TFLS|FLUSH */
    /* GFXH-1897: a pending L2T flush must complete before any further
     * L2TCACTL write or QPU traffic */
    for (i = 0; i < 2000000 && (v3d_core()[0x30 / 4] & (1u << 0)); i++)
        g2d_poll_hint();
    v3d_core()[0x24 / 4] = 0x0F0F0F0Fu;             /* SLCACTL */
    __asm__ __volatile__("dsb sy");
}

/* Flush the GPU texture L1/L2 caches so a reused canvas is not served
 * stale. */
static void g2d_invalidate_caches(void)
{
    g2d_invalidate_range(0u, ~0u);
}

/* Uniform-visibility barrier for PRE-elided dispatches (the middle
 * bands/tiles of a batched large-surface op).  The QPU's uniform fetch
 * is served through the V3D L2T/slice caches, so a dispatch that skips
 * the pre-job invalidation would re-read the PREVIOUS dispatch's
 * uniform block (still resident from its fetch) and re-run its
 * parameters - empirically every elided band re-rendered band 0 (only
 * the first band/tile ever landed).  The canvas data needs no
 * maintenance here (row/tile-disjoint, no CPU access between
 * dispatches, the first dispatch's PRE dropped the stale lines);
 * only the freshly-written 256-byte uniform block must be pushed out
 * and dropped from the GPU caches.  A ranged mode-0 L2T flush over
 * those few lines costs microseconds, unlike a full-L2 walk. */
static void g2d_uniform_fresh(void)
{
    __asm__ __volatile__("dsb sy");            /* _unif writes -> DRAM */
    g2d_invalidate_range(_unif_p,
                         _unif_p + CSD_UNIF_WORDS * 4u);
}

/* Hull of the caller's maint ranges: the smallest [lo, hi) IOVA
 * interval covering every known segment.  Returns 0 when no segment
 * is usable (the caller then falls back to the full-range walk).  The
 * hi computation is 64-bit and clamps to ~0u, so a segment ending at
 * the top of the IOVA space cannot wrap. */
static int g2d_maint_hull(const v3d_g2d_maint_t *m, uint32_t *lo, uint32_t *hi)
{
    uint64_t l = (uint64_t)~0u, h = 0;

    if (m == NULL)
        return 0;
    if (m->src_phy != 0 && m->src_span != 0) {
        uint64_t e = (uint64_t)m->src_phy + m->src_span;
        if (m->src_phy < l) l = m->src_phy;
        if (e > h) h = e;
    }
    if (m->dst_phy != 0 && m->dst_span != 0) {
        uint64_t e = (uint64_t)m->dst_phy + m->dst_span;
        if (m->dst_phy < l) l = m->dst_phy;
        if (e > h) h = e;
    }
    if (h == 0 || l >= h)
        return 0;
    if (h > (uint64_t)~0u)
        h = ~0u;
    *lo = (uint32_t)l;
    *hi = (uint32_t)h;
    return 1;
}

/* SMS power-up + reset kick: without it the QPU never launches. */
static void g2d_sms_powerup(void)
{
    volatile uint32_t *ree = _v3d + (V3D_SMS_OFF / 4);
    volatile uint32_t *tee = _v3d + ((V3D_SMS_OFF + 0x400u) / 4);
    uint32_t s, spins = 0;

    *tee = (1u << 29);                          /* CLEAR_POWER_OFF */
    __asm__ __volatile__("dsb sy");
    do {
        s = *tee & 0xFu;
    } while (s != 0x0u && ++spins < 1000000u);  /* wait IDLE */
    *ree = 0x4u;                                /* kick SMS reset */
    __asm__ __volatile__("dsb sy");
    spins = 0;
    do {
        s = *ree & 0xFu;
    } while ((s == 0xau || s == 0xbu) &&        /* ISOLATING/RESETTING */
             ++spins < 1000000u);
    /* AXI config: max burst length (kernel writes this after reset) */
    v3d_hub()[0x00 / 4] = 0xFu;
    __asm__ __volatile__("dsb sy");
}

static int g2d_mmu_map_range(ewokos_addr_t base, ewokos_addr_t top)
{
    uint64_t bytes, iova, pfn;
    uint32_t first, pages, i;

    if (top <= base)
        return 0;
    bytes = top - base;
    iova = (uint32_t)base;
    pfn = base >> 12;
    if (bytes > V3D_MMU_ADDR_LIMIT || iova + bytes > V3D_MMU_ADDR_LIMIT ||
        pfn >= V3D_MMU_PFN_LIMIT ||
        ((top - 1u) >> 12) >= V3D_MMU_PFN_LIMIT)
        return -1;

    first = (uint32_t)(iova >> 12);
    pages = (uint32_t)((bytes + 4095u) >> 12);
    if ((uint64_t)first + pages > V3D_MMU_PAGES)
        return -1;
    for (i = 0; i < pages; i++) {
        uint32_t pte = V3D_MMU_PTE_VALID | V3D_MMU_PTE_WRITEABLE |
                       (uint32_t)(pfn + i);

        /* Two physical ranges must never alias to the same 32-bit IOVA. */
        if (_mmu_pt[first + i] != 0 && _mmu_pt[first + i] != pte)
            return -1;
        _mmu_pt[first + i] = pte;
    }
    return 0;
}

/* Install the same V3D-local MMU model used by the upstream Linux driver.
 * The low 32 bits of a physical address are used as its IOVA; each PTE keeps
 * the full physical page number, so the high sys_dma carve-outs on 8GB boards
 * remain usable.  The kernel image/kmalloc area is deliberately absent. */
static int g2d_mmu_enable(void)
{
    volatile uint32_t *hub = v3d_hub();
    ewokos_addr_t raw_v, raw_p;
    uint32_t ctl, i;

    raw_v = dma_alloc(0, V3D_MMU_PT_BYTES + 4095u);
    if (raw_v == 0)
        return -1;
    raw_v = (raw_v + 4095u) & ~(ewokos_addr_t)4095u;
    raw_p = dma_phy_addr(0, raw_v);
    if (raw_p == 0 || (raw_p >> 12) >= (1ull << 32))
        return -1;
    _mmu_pt = (uint32_t *)(uintptr_t)raw_v;
    _mmu_pt_p = raw_p;

    raw_v = dma_alloc(0, 4096u + 4095u);
    if (raw_v == 0)
        return -1;
    raw_v = (raw_v + 4095u) & ~(ewokos_addr_t)4095u;
    raw_p = dma_phy_addr(0, raw_v);
    if (raw_p == 0 || (raw_p >> 12) >= (1ull << 31))
        return -1;
    _mmu_illegal_p = raw_p;
    memset((void *)(uintptr_t)raw_v, 0, 4096u);

    memset(_mmu_pt, 0, V3D_MMU_PT_BYTES);
    if (g2d_mmu_map_range(_ram_dma_base, _ram_dma_top) != 0 ||
        g2d_mmu_map_range(_ram_contig_base, _ram_contig_top) != 0)
        return -1;
    _mmu_pt[0] = 0; /* address zero is special throughout V3D */
    __asm__ __volatile__("dsb sy");

    _mmu_debug_info = hub[V3D_MMU_DEBUG_INFO / 4];
    _mmu_va_width = 30u + ((_mmu_debug_info >> 4) & 0xfu);
    if (_mmu_va_width < 32u || _mmu_va_width > 63u)
        return -1;

    hub[V3D_MMU_PT_PA_BASE / 4] = (uint32_t)(_mmu_pt_p >> 12);
    hub[V3D_MMU_ILLEGAL_ADDR / 4] =
        (uint32_t)(_mmu_illegal_p >> 12) | V3D_MMU_ILLEGAL_ENABLE;
    /* Bit 8 is reserved on V3D 7.1.  In particular it is not a write
     * enable: upstream Linux leaves it clear and controls write access
     * through each PTE's WRITEABLE bit.  Do not carry the old bare-metal
     * probe's undocumented bit into production D0 silicon. */
    ctl = V3D_MMU_ENABLE |
          V3D_MMU_PT_INVALID_ENABLE |
          V3D_MMU_PT_INVALID_ABORT |
          V3D_MMU_PT_INVALID_INT |
          V3D_MMU_WRITE_ABORT |
          V3D_MMU_WRITE_INT |
          V3D_MMU_CAP_ABORT |
          V3D_MMU_CAP_INT;
    hub[V3D_MMU_CTL / 4] = ctl;
    hub[V3D_MMUC_CONTROL / 4] = V3D_MMUC_ENABLE;
    hub[V3D_MMUC_CONTROL / 4] = V3D_MMUC_ENABLE | V3D_MMUC_FLUSH;
    for (i = 0; i < 2000000u; i++) {
        if (!(hub[V3D_MMUC_CONTROL / 4] & V3D_MMUC_FLUSHING))
            break;
    }
    if (i == 2000000u)
        return -1;

    hub[V3D_MMU_CTL / 4] |= V3D_MMU_TLB_CLEAR;
    for (i = 0; i < 2000000u; i++) {
        if (!(hub[V3D_MMU_CTL / 4] & V3D_MMU_TLB_CLEARING))
            break;
    }
    if (i == 2000000u)
        return -1;
    __asm__ __volatile__("dsb sy");

    slog("g2d: V3D MMU enabled pt=0x%x%08x ctl=0x%x mmuc=0x%x "
         "debug=0x%x va=%u pa=%u\r\n",
         (uint32_t)(_mmu_pt_p >> 32), (uint32_t)_mmu_pt_p,
         (uint32_t)hub[V3D_MMU_CTL / 4],
         (uint32_t)hub[V3D_MMUC_CONTROL / 4], _mmu_debug_info,
         _mmu_va_width, 30u + ((_mmu_debug_info >> 8) & 0xfu));
    slog("g2d: V3D IOVA map dma=0x%x%08x-0x%x%08x "
         "contig=0x%x%08x-0x%x%08x\r\n",
         (uint32_t)(_ram_dma_base >> 32), (uint32_t)_ram_dma_base,
         (uint32_t)(_ram_dma_top >> 32), (uint32_t)_ram_dma_top,
         (uint32_t)(_ram_contig_base >> 32), (uint32_t)_ram_contig_base,
         (uint32_t)(_ram_contig_top >> 32), (uint32_t)_ram_contig_top);
    return 0;
}

static int g2d_mmu_check_fault(int kern, int nunifs, int num_qpus)
{
    volatile uint32_t *hub = v3d_hub();
    volatile uint32_t *core = v3d_core();
    uint32_t ctl = hub[V3D_MMU_CTL / 4];
    uint32_t hub_int = hub[HUB_INT_STS / 4];
    uint32_t vio_reg, vio_id, client_id, pte = 0;
    int recoverable;
    uint64_t vio;
    const char *client;
    static int recoverable_reported;
    static int fatal_detail_reported;
    static const char *const names[KERN_N] = {
        "fill", "blit", "alpha", "rotate", "scale2", "rot90",
        "copy4", "fill4", "gauss_h5", "gauss_v5", "gauss_h9", "gauss_v9",
        "gauss_h3", "gauss_v3", "gauss_h7", "gauss_v7"
    };
    uint32_t faults = V3D_MMU_WRITE_FAULT |
                      V3D_MMU_PT_INVALID_FAULT |
                      V3D_MMU_CAP_FAULT;

    if (!(ctl & faults))
        return 0;
    vio_reg = hub[V3D_MMU_VIO_ADDR / 4];
    vio_id = hub[V3D_MMU_VIO_ID / 4];
    client_id = vio_id & V3D_MMU_VIO_ID_MASK;
    vio = (uint64_t)vio_reg << (_mmu_va_width - 32u);
    if ((vio >> 12) < V3D_MMU_PAGES)
        pte = _mmu_pt[vio >> 12];
    client = client_id < 0x30u ? "L2T" :
             (client_id < 0x38u ? "CLE" :
              (client_id == 0x38u ? "PTB" :
               (client_id == 0x39u ? "PSE" :
                (client_id == 0x3au ? "CSD" : "other"))));
    /* V3D 7.1 IP revision 10 emits a trailing L2T request with a bogus
     * 36-bit VA after otherwise-complete production CSD kernels.  The same
     * request is reproducible in the bare-metal MMU/canary harness: the
     * destination is complete and no protected RAM changes.  Without MMU
     * containment it becomes the low-RAM corruption seen on the 2GB board.
     *
     * Keep the MMU's invalid-PTE abort/redirect as the containment boundary.
     * Once CSD_DONE has arrived, only this exact IP-revision signature may
     * continue to the mandatory L2 writeback below.  A write violation, cap
     * fault, or a fault from any non-L2T client remains fatal. */
    recoverable = _trailing_l2t_quirk &&
                  client_id < 0x30u &&
                  (ctl & V3D_MMU_PT_INVALID_FAULT) != 0 &&
                  (ctl & (V3D_MMU_WRITE_FAULT | V3D_MMU_CAP_FAULT)) == 0 &&
                  (hub_int & HUB_INT_MMU_PTI) != 0 &&
                  (hub_int & (HUB_INT_MMU_WRV | HUB_INT_MMU_CAP)) == 0;
    /* The trailing request can occur after every dispatch.  Record it once
     * instead of turning a successful benchmark into an unbounded kernel-log
     * stream; fatal faults remain visible on every occurrence. */
    if (!recoverable || !recoverable_reported) {
        slog("g2d: V3D MMU fault kernel=%s ctl=0x%x id=0x%x(%s) "
             "hub=0x%x vio_reg=0x%x va=0x%x%08x pte=0x%x action=%s\r\n",
             (kern >= 0 && kern < KERN_N) ? names[kern] : "unknown",
             ctl, vio_id, client, hub_int, vio_reg, (uint32_t)(vio >> 32),
             (uint32_t)vio, pte,
             recoverable ? "redirected-continue" : "fatal");
        if (recoverable)
            recoverable_reported = 1;
    }
    if (!recoverable && !fatal_detail_reported) {
        fatal_detail_reported = 1;
        slog("g2d: fault context nq=%u nunif=%u code=0x%x unif=0x%x "
             "scratch=0x%x u6=0x%x u14=0x%x\r\n",
             (uint32_t)num_qpus, (uint32_t)nunifs, _kcode_p[kern],
             _unif_p, _scratch_p,
             nunifs > 6 ? _unif[6] : 0u,
             nunifs > 14 ? _unif[14] : 0u);
        slog("g2d: fault CSD status=0x%x current5=0x%x current6=0x%x "
             "current7=0x%x queued5=0x%x queued6=0x%x queued7=0x%x\r\n",
             (uint32_t)core[CSD_STATUS / 4],
             (uint32_t)core[CSD_CURRENT_CFG5 / 4],
             (uint32_t)core[CSD_CURRENT_CFG6 / 4],
             (uint32_t)core[CSD_CURRENT_CFG7 / 4],
             (uint32_t)core[(CSD_QUEUED_CFG0 + 5u * 4u) / 4],
             (uint32_t)core[(CSD_QUEUED_CFG0 + 6u * 4u) / 4],
             (uint32_t)core[(CSD_QUEUED_CFG0 + 7u * 4u) / 4]);
        slog("g2d: fault MMU hit=%u miss=%u stalls=%u\r\n",
             (uint32_t)hub[V3D_MMU_HIT / 4],
             (uint32_t)hub[V3D_MMU_MISSES / 4],
             (uint32_t)hub[V3D_MMU_STALLS / 4]);
    }
    hub[V3D_MMU_CTL / 4] = ctl; /* sticky fault bits are write-one-to-clear */
    hub[HUB_INT_CLR / 4] = hub_int &
        (HUB_INT_MMU_PTI | HUB_INT_MMU_WRV | HUB_INT_MMU_CAP);
    __asm__ __volatile__("dsb sy");
    return recoverable ? 0 : -1;
}

static int g2d_probe(void)
{
    uint32_t id0 = v3d_hub()[HUB_IDENT0 / 4];

    return (id0 == 0x42554856u) ? 1 : 0;        /* IDENT0 == "VHUB" */
}

/* Reset the V3D block via the PM power domain (assert/deassert
 * V3DRSTN); without the power-cycle the QPU array never launches.
 * Settle delays go through g2d_delay_us == usleep(): only lower bounds
 * are required here (tens / ~200 us), so tick-rounded sleeps are fine. */
#if !G2D_SKIP_PM_RESET
static void g2d_pm_reset(void)
{
    volatile uint32_t *pg = _pm + (PM_GRAFX_OFF / 4);
    uint32_t v = *pg;

    slog("g2d: V3D PM reset pre=0x%x\r\n", v);
    *pg = PM_PASSWORD | (v & ~PM_V3DRSTN);      /* assert V3D reset */
    __asm__ __volatile__("dsb sy");
    g2d_delay_us(20);
    v = *pg;
    *pg = PM_PASSWORD | (v & ~PM_V3DRSTN) | PM_V3DRSTN;
    __asm__ __volatile__("dsb sy");
    g2d_delay_us(200);
    slog("g2d: V3D PM reset post=0x%x\r\n", (uint32_t)*pg);
}
#endif

/* ---- CSD_DONE interrupt -------------------------------------------
 *
 * The kernel injects a registered irq handler into the daemon's MAIN
 * context (never into an ipc worker), on a private interrupt stack, with
 * the line disabled in the GIC until the handler's sys_interrupt_end.
 * The worker that owns the live dispatch therefore cannot be the one to
 * take the interrupt; it parks with proc_block_timeout() on a token and
 * the handler releases it with proc_wakeup_by().  The kernel latches a
 * wake that lands before the block (wake_pending, same-token), so the
 * check-then-block sequence below is race-free; the timed slice only
 * bounds a lost interrupt.
 *
 * Completion is signalled through _csd_irq_seq (bumped by the handler),
 * NOT by re-reading INT_STS: the handler must clear the status latch
 * itself (level line, otherwise the irq re-fires straight after the
 * kernel re-enables it), so a worker re-reading the latch after the
 * handler ran would see "not done".
 *
 * CSD_DONE is unmasked only around an IRQ-waiting dispatch: a spin-path
 * dispatch keeps today's proven poll loop and must not have its latch
 * stolen by the handler, and masking also keeps the interrupt load off
 * the short jobs where the handler round trip would cost more than the
 * dispatch itself.  Everything else in INT_MSK stays masked; the
 * handler acks whatever is latched so no foreign bit can hold the line.
 */
static interrupt_handler_t _csd_irq_handler;
static volatile uint32_t _csd_irq_seq;        /* CSD_DONE deliveries */
static volatile int32_t _csd_irq_waiter = -1; /* parked worker tid, or -1 */
static volatile int _csd_irq_on;              /* 0: poll only, 1: armed */
static uint32_t _csd_irq_miss_run;            /* consecutive silent completions */
#define CSD_IRQ_TOKEN ((ewokos_addr_t)(uintptr_t)&_csd_irq_seq)

static void g2d_csd_irq(uint32_t irq, ewokos_addr_t data)
{
    volatile uint32_t *core = v3d_core();
    uint32_t sts = core[INT_STS / 4];
    int32_t waiter;

    (void)irq;
    (void)data;
    if (sts != 0)
        core[INT_CLR / 4] = sts;      /* ack all: drop the level line */
    if (!(sts & INT_CSD_DONE))
        return;
    _csd_irq_seq++;
    __asm__ __volatile__("dmb ish" ::: "memory");
    waiter = _csd_irq_waiter;
    if (waiter >= 0)
        proc_wakeup_by(waiter, CSD_IRQ_TOKEN);
}

static void g2d_csd_irq_init(void)
{
    volatile uint32_t *core = v3d_core();

    core[INT_MSK_SET / 4] = ~0u;      /* nothing raises the line yet */
    core[INT_CLR / 4] = ~0u;
    __asm__ __volatile__("dsb sy");
    _csd_irq_handler.handler = g2d_csd_irq;
    _csd_irq_handler.data = 0;
    if (sys_interrupt_setup(V3D_CORE0_IRQ, &_csd_irq_handler) == 0) {
        _csd_irq_on = 1;
        slog("g2d: V3D CSD_DONE irq %u armed (msk=0x%x)\r\n",
             (uint32_t)V3D_CORE0_IRQ, (uint32_t)core[INT_MSK_STS / 4]);
    } else {
        slog("g2d: V3D CSD_DONE irq %u unavailable, polling\r\n",
             (uint32_t)V3D_CORE0_IRQ);
    }
}

/* Called under the run lock when the interrupt proved undeliverable. */
static void g2d_csd_irq_off(void)
{
    volatile uint32_t *core = v3d_core();

    _csd_irq_on = 0;
    core[INT_MSK_SET / 4] = ~0u;
    __asm__ __volatile__("dsb sy");
    sys_interrupt_setup(V3D_CORE0_IRQ, NULL);
    slog("g2d: V3D CSD_DONE irq %u never delivered, back to polling\r\n",
         (uint32_t)V3D_CORE0_IRQ);
}

int v3d_g2d_irq_enabled(void)
{
    return _csd_irq_on;
}

int v3d_g2d_init(void)
{
    sys_info_t si;
    ewokos_addr_t dev_va;
    uint32_t i;

    if (_inited)
        return _ok ? 0 : -1;
    _inited = 1;

    /* map the V3D block and the PM domain into this process (root
     * daemon; check_mem_map_arch whitelists both windows) */
    sys_get_sys_info(&si);
    /* Capture the only physical ranges the GPU may touch: sys_dma for
     * code/uniform staging and IPC_CONTIG shm for canvases.  Their low
     * 32 bits become V3D IOVAs; PTEs retain the full physical page number. */
    _ram_dma_base = si.sys_dma.phy_base;
    _ram_dma_top = si.sys_dma.phy_base + si.sys_dma.size;
    _ram_contig_base = si.shm_contig.phy_base;
    _ram_contig_top = si.shm_contig.phy_base + si.shm_contig.size;
    _dma_v_base = si.sys_dma.v_base;
    _dma_v_size = si.sys_dma.size;
    /* Dedicated device-window VAs: framebuffer.c fb_adopt() places the
     * scanout mapping at sys_dma.v_base+size in its own process; never
     * reuse that same VA range here. Overlapping dynamic VA slots across
     * processes are tolerable only while every switch reliably drops
     * stale TLB state (see __set_translation_table_base), so keep well
     * clear regardless: take a 64MB gap above the fb slot. */
#define G2D_DEV_VA_FB_GAP  (64u*1024u*1024u)
    dev_va = si.sys_dma.v_base + si.sys_dma.size + G2D_DEV_VA_FB_GAP;
    if (syscall3(SYS_MEM_MAP, dev_va, V3D_PHY_BASE, V3D_MAP_SIZE) != dev_va)
        return -1;
    if (syscall3(SYS_MEM_MAP, dev_va + V3D_MAP_SIZE,
                 PM_PHY_BASE, PM_MAP_SIZE) != dev_va + V3D_MAP_SIZE)
        return -1;
    _v3d = (volatile uint32_t *)(uintptr_t)dev_va;
    _pm = (volatile uint32_t *)(uintptr_t)(dev_va + V3D_MAP_SIZE);

    if (!g2d_probe())
        return -1;

    /* Clock setup is optional: keep the GPU usable at the firmware's
     * current rate if the property mailbox is unavailable. */
    g2d_clock_set_max();

    /* CSD staging in physically-contiguous dma memory (NOCACHE, so the
     * QPU sees the writes without ARM cache maintenance).  Kernels are
     * loaded once here; dispatches only refresh uniforms. */
    _ksrc[KERN_FILL] = g2d_qpu_argb_fill; _ksrc_n[KERN_FILL] = g2d_qpu_argb_fill_n;
    _ksrc[KERN_BLIT] = g2d_qpu_argb_blit; _ksrc_n[KERN_BLIT] = g2d_qpu_argb_blit_n;
    _ksrc[KERN_ALPHA] = g2d_qpu_argb_alpha; _ksrc_n[KERN_ALPHA] = g2d_qpu_argb_alpha_n;
    _ksrc[KERN_ROTATE] = g2d_qpu_argb_rotate; _ksrc_n[KERN_ROTATE] = g2d_qpu_argb_rotate_n;
    _ksrc[KERN_SCALE_POW2] = g2d_qpu_argb_scale_pow2; _ksrc_n[KERN_SCALE_POW2] = g2d_qpu_argb_scale_pow2_n;
    _ksrc[KERN_ROT90] = g2d_qpu_argb_rot90; _ksrc_n[KERN_ROT90] = g2d_qpu_argb_rot90_n;
    _ksrc[KERN_COPY] = g2d_qpu_argb_copy; _ksrc_n[KERN_COPY] = g2d_qpu_argb_copy_n;
    _ksrc[KERN_FILL4] = g2d_qpu_argb_fill4; _ksrc_n[KERN_FILL4] = g2d_qpu_argb_fill4_n;
    _ksrc[KERN_GAUSS_H5] = g2d_qpu_gauss_h5; _ksrc_n[KERN_GAUSS_H5] = g2d_qpu_gauss_h5_n;
    _ksrc[KERN_GAUSS_V5] = g2d_qpu_gauss_v5; _ksrc_n[KERN_GAUSS_V5] = g2d_qpu_gauss_v5_n;
    _ksrc[KERN_GAUSS_H9] = g2d_qpu_gauss_h9; _ksrc_n[KERN_GAUSS_H9] = g2d_qpu_gauss_h9_n;
    _ksrc[KERN_GAUSS_V9] = g2d_qpu_gauss_v9; _ksrc_n[KERN_GAUSS_V9] = g2d_qpu_gauss_v9_n;
    _ksrc[KERN_GAUSS_H3] = g2d_qpu_gauss_h3; _ksrc_n[KERN_GAUSS_H3] = g2d_qpu_gauss_h3_n;
    _ksrc[KERN_GAUSS_V3] = g2d_qpu_gauss_v3; _ksrc_n[KERN_GAUSS_V3] = g2d_qpu_gauss_v3_n;
    _ksrc[KERN_GAUSS_H7] = g2d_qpu_gauss_h7; _ksrc_n[KERN_GAUSS_H7] = g2d_qpu_gauss_h7_n;
    _ksrc[KERN_GAUSS_V7] = g2d_qpu_gauss_v7; _ksrc_n[KERN_GAUSS_V7] = g2d_qpu_gauss_v7_n;
    for (i = 0; i < KERN_N; i++) {
        uint32_t k;
        _kcode[i] = (uint64_t *)dma_alloc(0, CSD_CODE_WORDS * 8);
        if (_kcode[i] == 0)
            return -1;
        for (k = 0; k < _ksrc_n[i]; k++)
            _kcode[i][k] = _ksrc[i][k];
        _kcode_p[i] = (uint32_t)dma_phy_addr(0, (ewokos_addr_t)(uintptr_t)_kcode[i]);
        if (_kcode_p[i] == 0)
            return -1;
    }
    _unif = (uint32_t *)dma_alloc(0, CSD_UNIF_WORDS * 4);
    _scratch = (uint32_t *)dma_alloc(0, 4096u * 4u);
    if (_unif == 0 || _scratch == 0)
        return -1;
    _unif_p = (uint32_t)dma_phy_addr(0, (ewokos_addr_t)(uintptr_t)_unif);
    _scratch_p = (uint32_t)dma_phy_addr(0, (ewokos_addr_t)(uintptr_t)_scratch);
    if (_unif_p == 0 || _scratch_p == 0)
        return -1;
#if G2D_HW_PROBE_ONLY
    /* TEMP-BISECT: stop here - no SMS kick, no hub/AXI cfg, no L2C write.
     * The kernels sit in staging but V3D is left exactly as the firmware
     * booted it. */
    _ok = 1;
    return 0;
#endif
    g2d_sms_powerup();
#if !G2D_SKIP_PM_RESET
    g2d_pm_reset();      /* power-cycle GRAFX_V3D (QPU array launch fix) */
#endif
    {
        uint32_t ident1 = v3d_core()[CTL_IDENT1 / 4];
        uint32_t ident3 = v3d_hub()[HUB_IDENT3 / 4];
        uint32_t iprev = (ident3 >> 8) & 0xffu;
        uint32_t nslc = (ident1 >> 4) & 0xfu;
        uint32_t qpus_per_slice = (ident1 >> 8) & 0xfu;
        uint32_t detected = nslc * qpus_per_slice;

        _trailing_l2t_quirk = iprev == 10u;
        if (detected != 0 && detected <= 16u)
            _num_qpus = (int)detected;
        slog("g2d: V3D ident3=0x%x iprev=%u ident1=0x%x qpus=%u "
             "hw_qpus=%u vec4=probe l2t_tail=%s\r\n", ident3, iprev,
             ident1, (uint32_t)_num_qpus, detected,
             _trailing_l2t_quirk ? "redirect" : "fatal");
    }
    if (g2d_mmu_enable() != 0) {
        slog("g2d: V3D MMU setup failed\r\n");
        return -1;
    }
    g2d_l2c_enable();
    g2d_csd_irq_init();

    _ok = 1;
    return 0;
}

int v3d_g2d_ready(void)
{
    return _ok;
}

/* V3D clock rate in Hz confirmed at init (0 when unknown/unsupported) */
uint32_t v3d_g2d_clock_hz(void)
{
    return _v3d_clock_hz;
}

int v3d_g2d_num_qpus(void)
{
    return _num_qpus;
}

uint32_t v3d_g2d_scratch_phys(void)
{
    return _scratch_p;
}

/* Address validation gate: is [phy, phy+bytes) one of the RAM ranges
 * present in the V3D page table? */
int v3d_g2d_phy_valid(ewokos_addr_t phy, size_t bytes)
{
    ewokos_addr_t end;

    if (bytes == 0)
        return 0;
    end = phy + bytes;
    if (end <= phy)                 /* wrap */
        return 0;

    if (_ram_dma_top > _ram_dma_base &&
        phy >= _ram_dma_base && end <= _ram_dma_top)
        return 1;
    if (_ram_contig_top > _ram_contig_base &&
        phy >= _ram_contig_base && end <= _ram_contig_top)
        return 1;
    return 0;
}

/* One-shot hardware probe of the vec4 (TMUC general-access) path used
 * by argb_copy/argb_fill4: copy a 512-byte pattern between two scratch
 * regions on ONE QPU and verify it on the CPU.  The TMUC config
 * protocol is proven on the simulator but this hand-rolled CSD form is
 * unverified silicon territory, so a failed probe simply parks the fast
 * kernels (callers fall back to the single-word paths) instead of
 * shipping corrupted pixels. */
int v3d_g2d_vec4_ok(void)
{
    static int cached = -1;
    uint32_t u[10];
    uint32_t i;
    int rc;

    if (cached >= 0)
        return cached;
    if (!_ok)
        return 0;
    /* pattern in scratch[0..511], destination at scratch+4096; both
     * regions sit above the 3 KiB tail-redirect area only when V16=256,
     * which this probe uses (all 16 lanes valid, no redirect) */
    for (i = 0; i < 128; i++)
        _scratch[i] = 0x51C40000u + i * 0x101u;
    for (i = 0; i < 128; i++)
        _scratch[1024 + i] = 0xDEADBEEFu;
    u[0] = _scratch_p + 4096u;      /* dst */
    u[1] = _scratch_p;              /* src */
    u[2] = 256u;                    /* dstride */
    u[3] = 256u;                    /* sstride */
    u[4] = 0u;                      /* C = 0: single tail chunk per row */
    u[5] = 256u;                    /* V16 = 256: all 16 lanes valid */
    u[6] = 2u;                      /* H */
    u[7] = 2u;                      /* rpq */
    u[8] = 512u;                    /* drows */
    u[9] = 512u;                    /* srows */
    /* u10 = scratch base is appended by v3d_g2d_run (unused: V16=256) */
    {
        /* both probe buffers live in the scratch dma region */
        v3d_g2d_maint_t pm;
        pm.src_phy = _scratch_p;            pm.src_span = 512;
        pm.dst_phy = _scratch_p + 4096u;    pm.dst_span = 512;
        rc = v3d_g2d_run(g2d_qpu_argb_copy, (int)g2d_qpu_argb_copy_n, u, 10, 1,
                         _scratch, 512, _scratch + 1024, 512,
                         &pm, V3D_G2D_MAINT_ALL);
    }
    cached = (rc == 0);
    if (!cached)
        slog("g2d vec4 probe: dispatch rc=%d\n", rc);
    for (i = 0; cached && i < 128; i++) {
        if (_scratch[1024 + i] != 0x51C40000u + i * 0x101u) {
            slog("g2d vec4 probe: word %u got %08x want %08x\n", i,
                 _scratch[1024 + i], 0x51C40000u + i * 0x101u);
            cached = 0;
        }
    }
    if (cached)
        slog("g2d vec4 probe: TMUC vec4 general access ok\n");
    return cached;
}

/* ------------------------------------------------------------------ */
/* CSD dispatch                                                        */
/* ------------------------------------------------------------------ */

/* One dispatch at a time.  The CSD engine is single-issue: writing
 * CSD_QUEUED_CFG0 while a previous job is still live wedges the whole
 * machine (see the 213976ac field note - the QPU reads stale uniform
 * physical addresses and runs off into the AXI fabric, taking ARM,
 * RP1 networking and USB with it).  On top of that the uniform block
 * (_unif / _unif_p) and the TMU scratch (_scratch / _scratch_p) are
 * single-instance globals: two workers staging uniforms at once would
 * corrupt both jobs even if the CSD could queue them.
 *
 * The lock lives here, at the innermost dispatch, rather than in the
 * g2dd service or the bsp_g2d API layer, so that:
 *   - CPU-only paths (g2dd's g2d_cpu_blt tail, arch_g2d_* NEON) run
 *     fully parallel - they never touch this function and never see
 *     the lock;
 *   - any future caller of v3d_g2d_run (probe paths, new ops) is
 *     covered automatically instead of having to remember an outer
 *     lock contract.
 *
 * Banded / tiled large-surface ops hold it for the WHOLE op through
 * v3d_g2d_op_begin/end (the lock is owner-recursive, so the per-band
 * v3d_g2d_run calls inside the bracket just nest).  Interleaving bands
 * of two workers was cache-safe (every PRE's mode-0 walk writes back
 * the other op's dirty lines first), but it starved the short op: a
 * TTAS holder re-acquires in nanoseconds between its own bands while a
 * spinner only wins in the IPC gap between ops, so a 4-band 1080p
 * screen flush behind a g2dtest stream waited one whole foreign op per
 * band - 54 ms average against 0.6 ms idle, measured.  With the op
 * bracket it waits at most one foreign op once.
 *
 * Argument validation and the kernel-index lookup run BEFORE the lock:
 * they touch only caller-supplied data and the read-only _ksrc_n[]
 * table filled at init, so an invalid request never serializes behind
 * a live dispatch.
 *
 * A pthread_mutex, not a TTAS spinlock.  It is held across the whole
 * dispatch, including a long job's wait (the CSD_DONE irq park, or the
 * usleep(200) poll when the irq is off), and ewoksys spinlock.h forbids
 * exactly that: while the holder slept on the irq, every other g2dd
 * worker burned its core spinning on the word and starved the main
 * context that must run the irq handler - measured on a Pi 5 as every
 * 1080p band op 1.5-1.9x slower, 37 handler misses in 26684 waits and
 * displayd's blit_phy at 24 ms average.  The libewoksys mutex is a
 * futex-style word: uncontended lock/unlock is one CAS with no syscall,
 * and only a waiter that actually finds the lock held parks on a lazily
 * allocated kernel semaphore, so the holder's irq park no longer costs
 * anyone else a core.  The irq handler itself never takes it (it only
 * bumps the seq and posts the wake), so main-context injection cannot
 * deadlock against a parked worker. */
static pthread_mutex_t _v3d_run_lock = PTHREAD_MUTEX_INITIALIZER;
/* owner/depth make the lock recursive for the op bracket (the libewoksys
 * mutex is not).  Both are written only by the holder; a non-holder's
 * read sees -1 or a foreign tid (tids are unique and the holder clears
 * owner before the release), never its own, so the fast re-entry test
 * is race-free. */
static volatile int32_t _v3d_run_owner = -1;
static volatile uint32_t _v3d_run_depth = 0;

static void g2d_run_lock(void)
{
    int32_t me = thread_get_id();

    if (_v3d_run_depth != 0 && _v3d_run_owner == me) {
        _v3d_run_depth++;
        return;
    }
    pthread_mutex_lock(&_v3d_run_lock);
    _v3d_run_owner = me;
    _v3d_run_depth = 1;
}

static void g2d_run_unlock(void)
{
    if (--_v3d_run_depth != 0)
        return;
    _v3d_run_owner = -1;
    pthread_mutex_unlock(&_v3d_run_lock);
}

void v3d_g2d_op_begin(void)
{
    g2d_run_lock();
}

void v3d_g2d_op_end(void)
{
    g2d_run_unlock();
}

/* phase accounting (see v3d_g2d_stats_t); written only under
 * _v3d_run_lock */
static v3d_g2d_stats_t _stats;

static uint64_t g2d_now_ns(void)
{
    uint64_t ns = 0;

    (void)kernel_tic_nsec(&ns);
    return ns;
}

void v3d_g2d_stats_read(v3d_g2d_stats_t *out, int reset)
{
    g2d_run_lock();
    if (out != NULL)
        *out = _stats;
    if (reset)
        memset(&_stats, 0, sizeof(_stats));
    g2d_run_unlock();
}

int v3d_g2d_run(const uint64_t *code, int nwords,
                const uint32_t *unifs, int nunifs,
                int num_qpus,
                const void *src, size_t src_len,
                void *dst, size_t dst_len,
                const v3d_g2d_maint_t *maint, unsigned flags)
{
    volatile uint32_t *csd =
        _v3d + ((V3D_CORE0_OFF + CSD_QUEUED_CFG0) / 4);
    uint32_t cfg[8] = { 0 };
    uint32_t i;
    uint32_t poll_limit;
    uint32_t seq0 = 0;
    int use_irq, done;
    int kern = -1;
    int ret;
    uint64_t t0, t1, t2;

    if (code == NULL || nwords <= 0 || nwords > CSD_CODE_WORDS ||
        nunifs < 0 || nunifs >= CSD_UNIF_WORDS || num_qpus <= 0 || !_ok)
        return -1;
    /* POLL_IRQ rides on top of the caller's spin/pace choice: if the irq
     * is off or was retired between the caller's check and here, the
     * dispatch simply waits the way it would have without the flag. */
    poll_limit = (flags & V3D_G2D_POLL_YIELD) ?
                 CSD_POLL_YIELD_LIMIT : CSD_POLL_SPIN_LIMIT;

    /* select the preloaded kernel staging: no code is ever copied at
     * dispatch time - the CSD fetches the kernel straight from the dma
     * region it was preloaded into at init */
    if (code == g2d_qpu_argb_fill)
        kern = KERN_FILL;
    else if (code == g2d_qpu_argb_blit)
        kern = KERN_BLIT;
    else if (code == g2d_qpu_argb_alpha)
        kern = KERN_ALPHA;
    else if (code == g2d_qpu_argb_scale_pow2)
        kern = KERN_SCALE_POW2;
    else if (code == g2d_qpu_argb_rotate)
        kern = KERN_ROTATE;
    else if (code == g2d_qpu_argb_rot90)
        kern = KERN_ROT90;
    else if (code == g2d_qpu_argb_copy)
        kern = KERN_COPY;
    else if (code == g2d_qpu_argb_fill4)
        kern = KERN_FILL4;
    else if (code == g2d_qpu_gauss_h5)
        kern = KERN_GAUSS_H5;
    else if (code == g2d_qpu_gauss_v5)
        kern = KERN_GAUSS_V5;
    else if (code == g2d_qpu_gauss_h9)
        kern = KERN_GAUSS_H9;
    else if (code == g2d_qpu_gauss_v9)
        kern = KERN_GAUSS_V9;
    else if (code == g2d_qpu_gauss_h3)
        kern = KERN_GAUSS_H3;
    else if (code == g2d_qpu_gauss_v3)
        kern = KERN_GAUSS_V3;
    else if (code == g2d_qpu_gauss_h7)
        kern = KERN_GAUSS_H7;
    else if (code == g2d_qpu_gauss_v7)
        kern = KERN_GAUSS_V7;
    else
        return -1;      /* only the bsp_g2d kernels are supported */
    if ((uint32_t)nwords > _ksrc_n[kern])
        return -1;

    g2d_run_lock();
    t0 = g2d_now_ns();
    _stats.runs++;

    /* PRE: make the caller's ARM-side writes visible to the GPU, and
     * drop the ARM's stale copies of the destination.  NOCACHE dma
     * canvases need no maintenance. */
    if (flags & V3D_G2D_MAINT_PRE) {
        if (src && src_len && !is_dma_addr(src))
            g2d_dcache_clean((void *)src, src_len);
        if (dst && dst_len && !is_dma_addr(dst))
            g2d_dcache_clean_invalidate(dst, dst_len);
    }

    /* only the uniforms change per call; the kernel is already in dma */
    for (i = 0; i < (uint32_t)nunifs; i++)
        _unif[i] = unifs[i];
    /* extra trailing uniform: scratch base for the kernels' flush
     * epilogue (identity-mapped V3D address) */
    _unif[nunifs] = _scratch_p;
    if (flags & V3D_G2D_MAINT_PRE) {
        uint32_t lo, hi;

        /* Ranged PRE: the hull of the whole src/dst canvases covers
         * every address this job reads or writes (and, for a banded
         * op, every band's - the caller passes whole-canvas ranges).
         * The freshly-written uniform block sits outside the hull, so
         * its ranged barrier still runs first.  Unknown ranges fall
         * back to the full-L2 walk. */
        g2d_uniform_fresh();    /* stale-uniform guard, see above */
        if (g2d_maint_hull(maint, &lo, &hi)) {
            g2d_invalidate_range(lo, hi);
            _stats.pre_span += hi - lo;
        } else {
            g2d_invalidate_caches();
            _stats.pre_full++;
        }
    } else {
        g2d_uniform_fresh();    /* stale-uniform guard, see above */
        _stats.pre_skip++;
    }
    t1 = g2d_now_ns();
    _stats.pre_ns += t1 - t0;

    /* py-videocore7's proven Pi 5 config: cfg[0] = 1 workgroup in X,
     * cfg[3] = 0x000FF010, cfg[4] = batches = one per QPU */
    cfg[0] = 1u << 16;
    cfg[3] = 0x000FF010u;
    cfg[4] = (uint32_t)num_qpus;
    cfg[5] = _kcode_p[kern];    /* preloaded kernel's V3D IOVA */
    cfg[6] = _unif_p;
    cfg[7] = 0;
    use_irq = (flags & V3D_G2D_POLL_IRQ) != 0 && _csd_irq_on;
    if (use_irq) {
        /* Publish the waiter before the line can rise: the handler reads
         * it from the main context on whatever core takes the irq. */
        seq0 = _csd_irq_seq;
        _csd_irq_waiter = thread_get_id();
        __asm__ __volatile__("dsb sy" ::: "memory");
        v3d_core()[INT_MSK_CLR / 4] = INT_CSD_DONE;
    }
    for (i = 1; i <= 7; i++)
        csd[i] = cfg[i];
    csd[0] = cfg[0];            /* sole CFG0 write starts the dispatch */

    /* Every production kernel waits for pending TMU writes and then uses
     * the legal thread-end protocol, so CSD_DONE is authoritative. */
    done = 0;
    if (use_irq) {
        /* Park until the handler bumps the sequence (see the CSD_DONE
         * interrupt note above).  The status latch is consulted only
         * after a full timed slice elapsed without a delivery: by then a
         * set latch means the interrupt did not arrive, not that it is
         * still in flight. */
        uint64_t deadline = t1 + CSD_IRQ_WAIT_LIMIT_NS;
        int missed = 0;

        for (;;) {
            if (_csd_irq_seq != seq0) {
                done = 1;
                break;
            }
            if (g2d_now_ns() >= deadline)
                break;
            proc_block_timeout(CSD_IRQ_TOKEN, CSD_IRQ_WAIT_SLICE_US);
            if (_csd_irq_seq != seq0) {
                done = 1;
                break;
            }
            if (v3d_core()[INT_STS / 4] & INT_CSD_DONE) {
                done = 1;
                missed = 1;
                break;
            }
        }
        _csd_irq_waiter = -1;
        v3d_core()[INT_MSK_SET / 4] = INT_CSD_DONE;
        _stats.irq_waits++;
        if (missed) {
            _stats.irq_miss++;
            if (++_csd_irq_miss_run >= CSD_IRQ_MISS_MAX)
                g2d_csd_irq_off();
        } else if (done) {
            _csd_irq_miss_run = 0;
        }
    } else {
        /* A long job polls once per scheduler frame (about 1 ms at
         * timer_freq=1024), with a 256-frame timeout comparable to the
         * short job's spin bound. */
        for (i = 0; i < poll_limit; i++) {
            if (v3d_core()[INT_STS / 4] & INT_CSD_DONE)
                break;
            if (flags & V3D_G2D_POLL_YIELD)
                usleep(200);    /* one scheduler frame (~976us tick): >200 so it
                                 * parks instead of busy-spinning, <one tick so it
                                 * wakes on the first decrement.  usleep(1000)
                                 * would round up to two ticks; sched_yield()
                                 * would not pace at all. */
            else
                g2d_poll_hint();
        }
        done = i < poll_limit;
    }
    t2 = g2d_now_ns();
    _stats.exec_ns += t2 - t1;
    if (!done) {
        v3d_core()[INT_CLR / 4] = INT_CSD_DONE;
        (void)g2d_mmu_check_fault(kern, nunifs, num_qpus);
        /* A timeout is a real failure.  Do not reset the graphics domain
         * and do not replay this possibly-live operation. */
        _stats.timeouts++;
        ret = 1;
        goto out;
    }
    v3d_core()[INT_CLR / 4] = INT_CSD_DONE;
    if (g2d_mmu_check_fault(kern, nunifs, num_qpus) != 0) {
        ret = -1;
        goto out;
    }

    /* POST: GPU writes -> DRAM, then drop the ARM's stale destination
     * lines.  Ranged when the caller supplied a destination extent:
     * the clean spans the WHOLE dst canvas (for a banded op every
     * band's dirty lines) plus the TMU scratch (tail-redirect writes;
     * the CPU also reads it back in the vec4 probe).  Scratch lines
     * left dirty by other dispatches are harmless: scratch is a GPU
     * write sink, coherent through the L2T for its own reads. */
    if (flags & V3D_G2D_MAINT_POST) {
        if (maint != NULL && maint->dst_phy != 0 && maint->dst_span != 0) {
            uint64_t hi = (uint64_t)maint->dst_phy + maint->dst_span;
            uint32_t slo = _scratch_p;
            uint32_t shi = _scratch_p + CSD_SCRATCH_BYTES;
            uint32_t lo = maint->dst_phy;

            if (hi > (uint64_t)~0u)
                hi = ~0u;
            if (slo < lo)
                lo = slo;
            if (shi > (uint32_t)hi)
                hi = shi;
            g2d_flush_l2_range(lo, (uint32_t)hi);
            _stats.post_span += (uint32_t)hi - lo;
        } else {
            g2d_flush_l2();
            _stats.post_full++;
        }
        if (dst && dst_len && !is_dma_addr(dst))
            g2d_dcache_invalidate(dst, dst_len);
        _stats.post_ns += g2d_now_ns() - t2;
    } else {
        _stats.post_skip++;
    }
    ret = 0;
out:
    g2d_run_unlock();
    return ret;
}
