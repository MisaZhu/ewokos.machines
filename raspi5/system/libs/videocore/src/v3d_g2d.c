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
#include <unistd.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/sys.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
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
#define INT_CSD_DONE    (1u << 6)

/* ---- PM power domain (reset GRAFX_V3D) ---- */
#define PM_GRAFX_OFF 0x10cu
#define PM_PASSWORD  0x5A000000u
#define PM_V3DRSTN   (1u << 6)

#define CSD_CODE_WORDS 512   /* 344-word argb_alpha (endpoint-exact blend) */
#define CSD_UNIF_WORDS 64

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
#define KERN_N 8
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
static int _disable_vec4 = 0;
static int _d0_2g_quirk = 0;
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
 * timer access - never assume that in OS-portable code.  usleep() rounds
 * up to the kernel tick (~976us at timer_freq=1024), far above these
 * settle minimums and harmless for them. */
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

/* Write back dirty V3D caches to DRAM: flush the TMU write combiner,
 * then the L2T in CLEAN mode.  The clean must span the WHOLE L2T:
 * g2d_uniform_fresh() narrows L2TFLSTA/L2TFLEND to the uniform block
 * for the middle bands/tiles of a batched large-surface op and never
 * restores them, so reprogram the full range here - otherwise this
 * post-job flush writes back only those few uniform lines and the
 * canvas's dirty lines stay in the L2T, invisible to the CPU reading
 * DRAM (the batched op's final band/tile reads back stale). */
static void g2d_flush_l2(void)
{
    uint32_t i;

    v3d_core()[CTL_L2TCACTL / 4] = (1u << 8);               /* TMUWCF */
    for (i = 0; i < 2000000 && (v3d_core()[CTL_L2TCACTL / 4] & (1u << 8)); i++)
        g2d_poll_hint();
    v3d_core()[0x34 / 4] = 0;                       /* L2TFLSTA */
    v3d_core()[0x38 / 4] = ~0u;                     /* L2TFLEND */
    v3d_core()[CTL_L2TCACTL / 4] = (1u << 0) | (2u << 1);   /* L2TFLS | CLEAN */
    for (i = 0; i < 2000000 && (v3d_core()[CTL_L2TCACTL / 4] & (1u << 0)); i++)
        g2d_poll_hint();
    __asm__ __volatile__("dsb sy");
}

/* Flush the GPU texture L1/L2 caches so a reused canvas is not served
 * stale. */
static void g2d_invalidate_caches(void)
{
    uint32_t i;

    v3d_core()[0x34 / 4] = 0;                       /* L2TFLSTA */
    v3d_core()[0x38 / 4] = ~0u;                     /* L2TFLEND */
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

/* Uniform-visibility barrier for PRE-elided dispatches (the middle
 * bands/tiles of a batched large-surface op).  The QPU's uniform fetch
 * is served through the V3D L2T/slice caches, so a dispatch that skips
 * the full pre-job invalidation would re-read the PREVIOUS dispatch's
 * uniform block (still resident from its fetch) and re-run its
 * parameters - empirically every elided band re-rendered band 0 (only
 * the first band/tile ever landed).  The canvas data needs no
 * maintenance here (row/tile-disjoint, no CPU access between
 * dispatches, the first dispatch's full PRE dropped the stale lines);
 * only the freshly-written 256-byte uniform block must be pushed out
 * and dropped from the GPU caches.  A ranged mode-0 L2T flush over
 * those few lines costs microseconds, unlike the full-L2 walk. */
static void g2d_uniform_fresh(void)
{
    uint32_t i;

    __asm__ __volatile__("dsb sy");            /* _unif writes -> DRAM */
    v3d_core()[0x34 / 4] = _unif_p & ~63u;      /* L2TFLSTA */
    v3d_core()[0x38 / 4] =                      /* L2TFLEND */
        (_unif_p + CSD_UNIF_WORDS * 4u + 63u) & ~63u;
    v3d_core()[0x30 / 4] = (1u << 0) | (0u << 1);   /* L2TFLS | FLUSH */
    /* GFXH-1897: a pending L2T flush must complete before any further
     * L2TCACTL write or QPU traffic */
    for (i = 0; i < 2000000 && (v3d_core()[0x30 / 4] & (1u << 0)); i++)
        g2d_poll_hint();
    v3d_core()[0x24 / 4] = 0x0F0F0F0Fu;             /* SLCACTL */
    __asm__ __volatile__("dsb sy");
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
        "copy4", "fill4"
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
    /* BCM2712 D0 emits a trailing L2T request with a bogus 36-bit VA after
     * otherwise-complete production CSD kernels.  The same request is
     * reproducible in the bare-metal MMU/canary harness: the destination is
     * complete and no protected RAM changes.  With the MMU disabled that
     * transaction becomes the low-RAM corruption seen on the 2GB board.
     *
     * Keep the MMU's invalid-PTE abort/redirect as the containment boundary.
     * Once CSD_DONE has arrived, only this exact D0 signature may continue to
     * the mandatory L2 writeback below.  A write violation, cap fault, or a
     * fault from any non-L2T client remains fatal. */
    recoverable = _d0_2g_quirk &&
                  client_id < 0x30u &&
                  (ctl & V3D_MMU_PT_INVALID_FAULT) != 0 &&
                  (ctl & (V3D_MMU_WRITE_FAULT | V3D_MMU_CAP_FAULT)) == 0 &&
                  (hub_int & (HUB_INT_MMU_WRV | HUB_INT_MMU_CAP)) == 0;
    /* The D0 quirk can occur after every dispatch.  Record it once instead
     * of turning a successful benchmark into an unbounded kernel-log stream;
     * fatal faults remain visible on every occurrence. */
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
    /* BCM2712 D0 currently ships in the 2GB product.  Its TMUC vec4 path
     * can raise an abort during the capability probe, and an aborted TMU
     * sequence is not recoverable without a full GPU reset.  Keep the
     * proven scalar kernels on this variant instead of poisoning all
     * subsequent CSD jobs with a deliberately speculative probe. */
    _d0_2g_quirk = si.total_phy_mem_size <= (2ull << 30);
    _disable_vec4 = _d0_2g_quirk;
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
        uint32_t nslc = (ident1 >> 4) & 0xfu;
        uint32_t qpus_per_slice = (ident1 >> 8) & 0xfu;
        uint32_t detected = nslc * qpus_per_slice;

        if (detected != 0 && detected <= 16u)
            _num_qpus = (int)detected;
        /* Keep D0/2GB on one logical batch until its TIDX behaviour is
         * proven.  This bounds every scalar kernel to band zero and avoids
         * a bad physical thread ID turning into a far-out TMU address. */
        if (_disable_vec4)
            _num_qpus = 1;
        slog("g2d: V3D ident3=0x%x iprev=%u ident1=0x%x qpus=%u "
             "hw_qpus=%u vec4=%s\r\n", ident3, (ident3 >> 8) & 0xffu,
             ident1, (uint32_t)_num_qpus, detected,
             _disable_vec4 ? "off-2g-d0" : "probe");
    }
    if (g2d_mmu_enable() != 0) {
        slog("g2d: V3D MMU setup failed\r\n");
        return -1;
    }
    g2d_l2c_enable();

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
    if (_disable_vec4) {
        cached = 0;
        slog("g2d vec4 probe: disabled on BCM2712 D0/2GB\r\n");
        return cached;
    }
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
    rc = v3d_g2d_run(g2d_qpu_argb_copy, (int)g2d_qpu_argb_copy_n, u, 10, 1,
                     _scratch, 512, _scratch + 1024, 512,
                     V3D_G2D_MAINT_ALL);
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

int v3d_g2d_run(const uint64_t *code, int nwords,
                const uint32_t *unifs, int nunifs,
                int num_qpus,
                const void *src, size_t src_len,
                void *dst, size_t dst_len, unsigned maint)
{
    volatile uint32_t *csd =
        _v3d + ((V3D_CORE0_OFF + CSD_QUEUED_CFG0) / 4);
    uint32_t cfg[8] = { 0 };
    uint32_t i;
    int kern = -1;

    if (code == NULL || nwords <= 0 || nwords > CSD_CODE_WORDS ||
        nunifs < 0 || nunifs >= CSD_UNIF_WORDS || num_qpus <= 0 || !_ok)
        return -1;

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
    else
        return -1;      /* only the bsp_g2d kernels are supported */
    if ((uint32_t)nwords > _ksrc_n[kern])
        return -1;

    /* PRE: make the caller's ARM-side writes visible to the GPU, and
     * drop the ARM's stale copies of the destination.  NOCACHE dma
     * canvases need no maintenance. */
    if (maint & V3D_G2D_MAINT_PRE) {
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
    if (maint & V3D_G2D_MAINT_PRE)
        g2d_invalidate_caches();
    else
        g2d_uniform_fresh();    /* stale-uniform guard, see above */

    /* py-videocore7's proven Pi 5 config: cfg[0] = 1 workgroup in X,
     * cfg[3] = 0x000FF010, cfg[4] = batches = one per QPU */
    cfg[0] = 1u << 16;
    cfg[3] = 0x000FF010u;
    cfg[4] = (uint32_t)num_qpus;
    cfg[5] = _kcode_p[kern];    /* preloaded kernel's V3D IOVA */
    cfg[6] = _unif_p;
    cfg[7] = 0;
    for (i = 1; i <= 7; i++)
        csd[i] = cfg[i];
    csd[0] = cfg[0];            /* sole CFG0 write starts the dispatch */

    /* Every production kernel waits for pending TMU writes and then uses
     * the legal thread-end protocol, so CSD_DONE is authoritative. */
    for (i = 0; i < 2000000; i++) {
        if (v3d_core()[INT_STS / 4] & INT_CSD_DONE)
            break;
        g2d_poll_hint();
    }
    if (i == 2000000) {
        v3d_core()[INT_CLR / 4] = INT_CSD_DONE;
        (void)g2d_mmu_check_fault(kern, nunifs, num_qpus);
        /* A timeout is a real failure.  Do not reset the graphics domain
         * and do not replay this possibly-live operation. */
        return 1;
    }
    v3d_core()[INT_CLR / 4] = INT_CSD_DONE;
    if (g2d_mmu_check_fault(kern, nunifs, num_qpus) != 0)
        return -1;

    /* POST: GPU writes -> DRAM, then drop the ARM's stale destination
     * lines */
    if (maint & V3D_G2D_MAINT_POST) {
        g2d_flush_l2();
        if (dst && dst_len && !is_dma_addr(dst))
            g2d_dcache_invalidate(dst, dst_len);
    }
    return 0;
}
