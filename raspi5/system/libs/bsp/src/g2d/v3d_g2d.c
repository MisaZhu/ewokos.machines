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
 *     contiguous sys_dma memory), so the QPU fetches them by physical
 *     address without a V3D MMU page table;
 *   - canvases arrive as virtual addresses with their physical bases
 *     supplied by the caller (bsp_g2d *_phy); cache maintenance is
 *     skipped for NOCACHE dma canvases;
 *   - zero copy: canvas pixels are never copied - the kernels operate
 *     directly on the caller's buffers (physical addresses); the three
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
/* ---- core registers ---- */
#define CTL_L2CACTL  0x20u
#define CTL_L2TCACTL 0x30u

/* CSD dispatch (core0 window) */
#define CSD_QUEUED_CFG0 0x930u
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
 * again for the first real V3D experiments.  If the console dies right
 * after "pm GRAFX pre=", set back to 1. */
#define G2D_SKIP_PM_RESET 1

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
 * on the caller's canvas addresses (zero copy: the physical addresses
 * computed from the passed-in pointers are what the kernels write to). */
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

static int _inited = 0;
static int _ok = 0;

/* V3D clock rate in Hz as set by g2d_clock_set_max() at init (0 until
   confirmed, or when the property mailbox is unavailable) */
static uint32_t _v3d_clock_hz = 0;

/* physical RAM ranges captured at init, used by v3d_g2d_phy_valid to
 * reject caller-supplied *_phy values that would let the GPU (no MMU)
 * scribble over arbitrary memory */
static ewokos_addr_t _ram_alloc_base = 0, _ram_alloc_top = 0;
static ewokos_addr_t _ram_dma_base = 0, _ram_dma_top = 0;
static ewokos_addr_t _ram_contig_base = 0, _ram_contig_top = 0;
static ewokos_addr_t _ram_total = 0;

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

    *pg = PM_PASSWORD | (v & ~PM_V3DRSTN);      /* assert V3D reset */
    __asm__ __volatile__("dsb sy");
    g2d_delay_us(20);
    v = *pg;
    *pg = PM_PASSWORD | (v & ~PM_V3DRSTN) | PM_V3DRSTN;
    __asm__ __volatile__("dsb sy");
    g2d_delay_us(200);
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
    /* capture the physical RAM ranges the GPU may legally touch: the
     * allocable region, the sys_dma window and the IPC_CONTIG shm slab
     * (all 64-bit physical bases; only the sub-4 GB part is usable by
     * the 32-bit TMU addresses) */
    _ram_alloc_base = si.allocable_phy_mem_base;
    _ram_alloc_top = si.allocable_phy_mem_top;
    _ram_dma_base = si.sys_dma.phy_base;
    _ram_dma_top = si.sys_dma.phy_base + si.sys_dma.size;
    _ram_contig_base = si.shm_contig.phy_base;
    _ram_contig_top = si.shm_contig.phy_base + si.shm_contig.size;
    _ram_total = si.total_phy_mem_size;
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
    g2d_l2c_enable();
    /* NOTE: no V3D MMU page table - the proven path runs without it */

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
    return 12;              /* BCM2712 V3D 7.1 */
}

uint32_t v3d_g2d_scratch_phys(void)
{
    return _scratch_p;
}

/* Address validation gate: is [phy, phy+bytes) a legitimate physical
 * RAM range the GPU may write?  The QPU has no MMU, so a caller that
 * passes a bad *_phy would let the kernels scribble over arbitrary
 * memory; reject anything outside the known RAM regions. */
int v3d_g2d_phy_valid(ewokos_addr_t phy, size_t bytes)
{
    ewokos_addr_t end;

    if (bytes == 0)
        return 0;
    end = phy + bytes;
    if (end <= phy)                 /* wrap */
        return 0;

    if (_ram_alloc_top > _ram_alloc_base &&
        phy >= _ram_alloc_base && end <= _ram_alloc_top)
        return 1;
    if (_ram_dma_top > _ram_dma_base &&
        phy >= _ram_dma_base && end <= _ram_dma_top)
        return 1;
    if (_ram_contig_top > _ram_contig_base &&
        phy >= _ram_contig_base && end <= _ram_contig_top)
        return 1;
    /* fallback: below the total physical memory size */
    if (_ram_total != 0 && end <= _ram_total)
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
     * epilogue (physical address - the QPU has no MMU) */
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
    cfg[5] = _kcode_p[kern];    /* preloaded kernel, physical address */
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
        /* A timeout is a real failure.  Do not reset the graphics domain
         * and do not replay this possibly-live operation. */
        return 1;
    }
    v3d_core()[INT_CLR / 4] = INT_CSD_DONE;

    /* POST: GPU writes -> DRAM, then drop the ARM's stale destination
     * lines */
    if (maint & V3D_G2D_MAINT_POST) {
        g2d_flush_l2();
        if (dst && dst_len && !is_dma_addr(dst))
            g2d_dcache_invalidate(dst, dst_len);
    }
    return 0;
}
