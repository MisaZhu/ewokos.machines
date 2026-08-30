/*
 * v3d_g2d.c - VideoCore (V3D) hardware back end for the EwokOS raspix
 * bsp_g2d layer, ported from the raspi5 v3d_g2d.c bare-metal driver for
 * the VC4-generation VideoCore:
 *
 *   Pi 4 / CM4  BCM2711  V3D 4.2  (hub "VHUB" + core0, CSD compute)
 *   Pi 3 family BCM2837  V3D 2.1  (single-core, SRQ user programs)
 *
 * Differences from the raspi5 driver:
 *   - registers live INSIDE the shared 32 MB MMIO window (V3D at
 *     mmio_base+0xC00000, PM at mmio_base+0x100000), so the windows are
 *     reached through _mmio_base instead of SYS_MEM_MAP;
 *   - no SMS power-up / hub AXI kick: those blocks only exist on V3D
 *     7.x (raspi5); V3D 4.x is brought up by probe + clock + staging;
 *   - V3D 4.x CSD register block sits at 0x904..0x91c (the V3D 7.x
 *     layout sits at 0x930..0x94c) and CSD completion is INT bit 7
 *     instead of bit 6 (Linux drm/v3d v3d_regs.h, INT_CSDDONE(ver));
 *   - the VC4-generation CSD config follows Mesa's compute model
 *     (workgroup counts in CFG0-2, supergroup layout in CFG3,
 *     NUM_BATCHES-1 in CFG4, shader-record bits in CFG5), while the
 *     V3D 7.x path keeps the proven raspi5 register values;
 *   - V3D 2.1 has no usable CSD: the *_vc4 kernels run one SRQ thread
 *     per QPU with the proven GPU_FFT launch protocol (firmware tag
 *     0x30012 enables QPU access, SRQUA/SRQPC enqueue each thread,
 *     SRQCS counts completions).
 *
 * CSD code/uniform/scratch staging is dma_alloc'ed (physically
 * contiguous sys_dma memory), so the QPU fetches them by physical
 * address without a V3D MMU page table.  Canvases arrive as virtual
 * addresses with their physical bases supplied by the caller (bsp_g2d
 * *_phy); zero copy - the kernels operate directly on the caller's
 * buffers and a dispatch only refreshes the small uniform block.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sysinfo.h>
#include <ewoksys/sys.h>
#include <ewoksys/dma.h>
#include <ewoksys/mmio.h>
#include <ewoksys/klog.h>
#include <arch/bcm283x/mailbox.h>
#include "v3d_g2d.h"
#include "g2d_qpu_kernels.h"

/* VC bus address alias for uncached access (not exported by mailbox.h;
 * the other bcm283x drivers carry the same local define). */
#define MAILBOX_VC_ALIAS_NONCACHED 0x40000000u

/* VC bus alias for everything the QPU itself fetches or DMAs
 * (SRQUA/SRQPC addresses, canvas rows, TMU sources, scratch):
 * 0xC0000000 = coherent/direct, bypassing the VC L2 caches.  This is
 * exactly what GPU_FFT release 3.0 hands the V3D on Pi2/3 (mem_alloc
 * flag 0x4 = MEM_FLAG_COHERENT; its comment: "ARM cannot see VC4 L2
 * on Pi 2").  The 0x40000000 alias is VC-L2-CACHEABLE: the V3D L2T
 * served stale lines for freshly written uniforms and every
 * uniform-value probe saw garbage while uniform-free probes passed.
 * The mailbox property tags above keep the 0x40000000 alias (the
 * firmware owns that handshake). */
#define V3D_VC_ALIAS_DIRECT 0xC0000000u

/* ---- offsets inside the shared MMIO window (_mmio_base) ---- */
#define V3D_MMIO_OFF   0xC00000u   /* Pi3: 0x3fc00000, Pi4: 0xfec00000 */
#define PM_MMIO_OFF    0x100000u   /* Pi3: 0x3f100000, Pi4: 0xfe100000 */

/* ---- V3D register layout (Linux drm/v3d v3d_regs.h) ---- */
#define V3D_HUB_OFF    0x00000u
#define V3D_CORE0_OFF  0x08000u

/* hub registers (V3D >= 3.3 only; absent on V3D 2.1) */
#define HUB_IDENT0 0x08u           /* "VHUB" = 0x42554856 */
#define HUB_IDENT1 0x0cu           /* TVVER bits 3:0, REV bits 7:4 */
#define HUB_IDENT0_EXPECT 0x42554856u

/* core registers (same offsets on every V3D generation) */
#define CTL_IDENT0   0x00u         /* "V3D" (vc4: 0x02443356 family) */
#define CTL_IDENT1   0x04u         /* QUPS bits 11:8, NSLC bits 7:4 */
#define CTL_L2CACTL  0x20u          /* L2CCLR(2) L2CDIS(1) L2CENA(0) */
#define CTL_SLCACTL  0x24u          /* write counts to clear slice caches */
#define CTL_L2TCACTL 0x30u          /* TMUWCF(8) FLM(2:1) L2TFLS(0) */
#define CTL_L2TFLSTA 0x34u
#define CTL_L2TFLEND 0x38u
#define INT_STS      0x50u
#define INT_CLR      0x58u

/* CSD dispatch: offsets and the completion bit differ between the
 * V3D 7.x layout (0x930) and the older one (0x904); CFG0..CFG6 carry
 * the same fields on both. */
#define CSD_STATUS_OFF      0x900u
#define CSD_CFG_BASE_OLD    0x904u    /* V3D < 71 */
#define CSD_CFG_BASE_V7     0x930u    /* V3D >= 71 */
#define INT_CSDDONE_OLD     (1u << 7)
#define INT_CSDDONE_V7      (1u << 6)
/* CFG5 shader-record bits (VC4 generation; Mesa broadcom/common/v3d_csd.h
 * and the CL shader-state records): 4-way threadable program, single
 * segment (starts in the final thread section), propagate NaNs. */
#define CSD_CFG5_THREADING  (1u << 0)
#define CSD_CFG5_SINGLE_SEG (1u << 1)
#define CSD_CFG5_PROP_NANS  (1u << 2)

/* ---- VC4 (V3D 2.1) SRQ user-program launcher (GPU_FFT protocol) ----
 * On V3D 2.1 the core block sits at the hub offset (no hub block).
 * Writing SRQPC enqueues one thread for that QPU; SRQCS counts the
 * completions (bits 23:16). */
#define V3D_DBCFG    0xe00u
#define V3D_DBQITE   0xe2cu
#define V3D_DBQITC   0xe30u
#define V3D_SRQPC    0x430u          /* write = enqueue a QPU thread */
#define V3D_SRQUA    0x434u          /* per-QPU uniforms address */
#define V3D_SRQCS    0x43cu          /* 23:16 done, 15:8 req, 7 err */
/* GPU_FFT's reset value: clear the done counter (bit 16), the request
 * counter (bit 8) and the error flag (bit 7). */
#define V3D_SRQCS_CLEAR  ((1u << 16) | (1u << 8) | (1u << 7))

/* diagnostic reads around a wedged launch (Linux vc4_regs.h offsets) */
#define V3D_SQRSV1   0x414u          /* SRQ reserved state */
#define V3D_SQCNTL   0x418u          /* SRQ control */
#define V3D_ERRSTAT  0xf20u          /* error status latch */
#define V3D_VPMBASE  0x504u          /* [4:0] user VPM reservation, x256B */

/* ---- PM power domain ---- */
#define PM_GRAFX_OFF 0x10cu
#define PM_PASSWORD  0x5A000000u
#define PM_V3DRSTN   (1u << 6)

#define CSD_CODE_WORDS 256
#define CSD_UNIF_WORDS 512      /* VC4: 16 QPUs x VC4_UNIF_QWORDS slots */

/* Raspberry Pi firmware property tags and clock ID. */
#define FW_GET_CLOCK_RATE      0x00030002u
#define FW_GET_MAX_CLOCK_RATE  0x00030004u
#define FW_SET_CLOCK_RATE      0x00038002u
#define FW_SET_ENABLE_QPU      0x00030012u
#define FW_GET_DOMAIN_STATE    0x00030030u
#define FW_SET_DOMAIN_STATE    0x00038030u
#define FW_SET_POWER_STATE     0x00028001u   /* old interface */
#define FW_CLOCK_V3D           5u
/* firmware power-domain indices (Linux dt-bindings index + 1) */
#define FW_DOMAIN_V3D          11u
#define FW_OLD_DEVICE_V3D      10u           /* old SET_POWER_STATE id */
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

typedef struct {
    uint32_t buf_size;
    uint32_t code;
    struct {
        uint32_t tag;
        uint32_t value_buf_size;
        uint32_t value_len;
        uint32_t enable;
    } tag;
    uint32_t end_tag;
} __attribute__((packed)) g2d_qpu_enable_req_t;

typedef struct {
    uint32_t buf_size;
    uint32_t code;
    struct {
        uint32_t tag;
        uint32_t value_buf_size;
        uint32_t value_len;
        uint32_t domain;
        uint32_t on;
    } tag;
    uint32_t end_tag;
} __attribute__((packed)) g2d_domain_req_t;

/* BRING-UP SWITCH:
 *   G2D_HW_PROBE_ONLY
 *                   0 = full bring-up (probe -> staging -> clock ->
 *                       optional pm reset -> l2c enable)
 *                   1 = probe + dma staging only; no V3D register
 *                       writes at all */
#define G2D_HW_PROBE_ONLY 0

/* DEBUG SWITCH: PM_GRAFX.V3DRSTN power-cycle at mmio+0x100000.
 * The raspi5 driver REQUIRED the power-cycle for the V3D 7.x QPU array
 * to launch; on BCM2835/2711 the V3D domain is normally already powered
 * by the firmware (its register block is not documented publicly either),
 * so the write stays disabled until a real bring-up hang points at it. */
#define G2D_SKIP_PM_RESET 1

/* DEBUG SWITCH: at init, launch a 5-instruction nop kernel on VC4 to
 * separate "launch never completes" from "the real kernels hang". */
#define G2D_VC4_NOP_TEST 1

/* DEBUG SWITCH: PM_GRAFX.V3DRSTN power-cycle as the wedge recovery.
 * The FIRST hardware run with this enabled died with a black screen
 * and a full machine freeze, and the raspi5 driver carries the same
 * warning ("writing PM_GRAFX.V3DRSTN can hang the Device write /
 * dsb"), so the recovery stays gated until the read-only bisect
 * probes (D1-D3) and the register dumps identify the wedge and the
 * PM write is proven safe on this silicon.  When 0, g2d_vc4_recover
 * only logs and leaves the wedge in place (the previous behaviour,
 * which never killed the box). */
#define G2D_PM_RESET_RECOVERY 0

/* DEBUG SWITCH: the one-shot wedge-bisection probes at init (the W
 * round).  Result on real Pi3 hardware: D2/W1/W2/W3/W4/W5/W6 all ok,
 * W2b TIMEOUT exactly as designed (split A/B register files proven;
 * uniform values proven delivered via the 0xC0000000 direct alias).
 * They now stay OFF: W2b's deliberate spin permanently occupies one
 * QPU (PM recovery is gated) and production dispatch needs all 12. */
#define G2D_VC4_PROBES 0

/* nop kernel: exits immediately - used to separate "QPU launch never
 * completes" from "the dispatched kernels hang". */
static const uint64_t g2d_nop_vc4[] = {
    0x100009e7009e7000ULL,  /* nop */
    0x100009e7009e7000ULL,  /* nop */
    0x300009e7009e7000ULL,  /* thread end */
    0x100009e7009e7000ULL,  /* nop */
    0x100009e7009e7000ULL,  /* nop */
};

/* forward declaration (init-time nop probe uses it before the SRQ
 * dispatch section defines it) */
static int g2d_vc4_launch(uint32_t code_p, uint32_t unif_p, uint32_t nq,
                          uint32_t *srqcs_out);
static int g2d_vc4_launch_capture(uint32_t code_p, uint32_t unif_p,
                                  uint32_t nq, uint32_t cap[4]);
static void g2d_vc4_dump_regs(const char *tag);
static int g2d_vc4_recover(void);

/* ---- mapped register windows (inside the shared MMIO window) ---- */
static volatile uint32_t *_v3d;   /* V3D block (hub at +0, core at +0x8000) */
static volatile uint32_t *_pm;    /* PM power domain */

static inline volatile uint32_t *v3d_hub(void)
{
    return _v3d + (V3D_HUB_OFF / 4);
}

static inline volatile uint32_t *v3d_core(void)
{
    return _v3d + (V3D_CORE0_OFF / 4);
}

/* ---- arch-portable barriers (this file builds for arm AND aarch64) ----
 * The arm build target defaults to a pre-ARMv7 assembler, where the
 * `dsb` mnemonic does not exist - use the DSB encoding via MCR, valid
 * on every ARMv6+ target. */
static inline void g2d_dsb(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dsb sy" ::: "memory");
#elif defined(__arm__)
    __asm__ __volatile__("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
#endif
}

/* ---- dma staging (physically contiguous, NOCACHE) ----
 * The eight ARGB kernels (four V3D 4.2 CSD + four VC4 SRQ) are
 * PRELOADED once at init into their own 256-word dma regions, so a
 * dispatch never re-copies code - only the small uniform block is
 * refreshed per call.  The GPU operates directly on the caller's
 * canvas addresses (zero copy). */
#define KERN_FILL 0
#define KERN_BLIT 1
#define KERN_ALPHA 2
#define KERN_ROTATE 3
#define KERN_FILL_VC4 4
#define KERN_BLIT_VC4 5
#define KERN_ALPHA_VC4 6
#define KERN_ROTATE_VC4 7
#define KERN_TOTAL 8
static uint64_t *_kcode[KERN_TOTAL];        /* per-kernel code VA (dma) */
static uint32_t _kcode_p[KERN_TOTAL];       /* per-kernel code physical */
static const uint64_t *_ksrc[KERN_TOTAL];   /* kernel source arrays */
static unsigned _ksrc_n[KERN_TOTAL];        /* kernel instruction counts */
static uint32_t *_unif;            /* uniform staging (CSD_UNIF_WORDS) */
static uint32_t *_scratch;         /* TMU write scratch (16 KiB) */
static uint32_t _unif_p, _scratch_p;
static uint64_t *_nop_code;        /* dma staging of g2d_nop_vc4 */
static uint32_t _nop_code_p;
static uint64_t *_diag_code[22];   /* dma staging of the diag_*_vc4 */
static uint32_t _diag_code_p[22];

static int _inited = 0;
static int _ok = 0;
static int _ver = 0;               /* architecture version x10 */
static int _num_qpus = 0;
static int _has_hub = 0;

/* Core (control) register block: hub-style V3D >= 3.3 keeps the core
 * at base+0x8000 next to the hub; V3D 2.1 (VC4) has no hub and the
 * whole core block (IDENT, L2C, SRQ, ...) sits at base+0x0000 - the
 * layout the Linux vc4 driver uses. */
static inline volatile uint32_t *v3d_ctl(void)
{
    return _has_hub ? v3d_core() : v3d_hub();
}

/* physical RAM ranges captured at init, used by v3d_g2d_phy_valid to
 * reject caller-supplied *_phy values that would let the GPU (no MMU)
 * scribble over arbitrary memory */
static ewokos_addr_t _ram_alloc_base = 0, _ram_alloc_top = 0;
static ewokos_addr_t _ram_dma_base = 0, _ram_dma_top = 0;
static ewokos_addr_t _ram_contig_base = 0, _ram_contig_top = 0;
static ewokos_addr_t _ram_total = 0;

/* CACHE-MAINTENANCE CONTRACT (arm + aarch64 portable):
 *
 * Every buffer handed to this driver that the QPU may read or write
 * must be mapped Non-Cacheable in each process.  The contig shm slab is
 * mapped NOCACHE by the kernel for exactly this reason, and gpu_phys()
 * only accepts canvases flagged contig - so both directions of
 * CPU<->GPU visibility go straight through DRAM and NO maintenance by
 * virtual address is needed; a dsb orders the writes.
 *
 * The GPU-side caches do NOT snoop CPU writes, so every job still
 * invalidates the V3D L2 + slice caches before dispatch and flushes
 * the L2T afterwards (see g2d_invalidate_caches / g2d_flush_l2), the
 * same discipline the Linux vc4 driver uses (vc4_flush_caches). */

/* is the pointer inside the sys_dma NOCACHE window? (no cache
 * maintenance needed there) */
static int is_dma_addr(const void *v)
{
    sys_info_t si;
    uintptr_t a = (uintptr_t)v;

    sys_get_sys_info(&si);
    return a >= (uintptr_t)si.sys_dma.v_base &&
           a < (uintptr_t)(si.sys_dma.v_base + si.sys_dma.size);
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
    /* ewokos_addr_t is 32 bits on the arm build: always sub-4 GB */
    if (phys != 0 && (sizeof(phys) <= 4 || (phys >> 32) == 0)) {
        memset(&msg, 0, sizeof(msg));
        msg.data = (((uint32_t)phys | MAILBOX_VC_ALIAS_NONCACHED) >> 4);
        msg.channel = PROPERTY_CHANNEL;
        if (bcm283x_mailbox_call_timeout(&msg, 0) == 0 &&
            (req->code & FW_RESPONSE) != 0 &&
            (req->tag.value_len & FW_RESPONSE) != 0 &&
            (req->tag.value_len & ~FW_RESPONSE) >= 8 &&
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
    if (phys != 0 && (sizeof(phys) <= 4 || (phys >> 32) == 0)) {
        memset(&msg, 0, sizeof(msg));
        msg.data = (((uint32_t)phys | MAILBOX_VC_ALIAS_NONCACHED) >> 4);
        msg.channel = PROPERTY_CHANNEL;
        if (bcm283x_mailbox_call_timeout(&msg, 0) == 0 &&
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

    if (bcm283x_mailbox_init() == 0 ||
        g2d_clock_get(FW_GET_MAX_CLOCK_RATE, &max_hz) != 0 ||
        g2d_clock_set(max_hz) != 0 ||
        g2d_clock_get(FW_GET_CLOCK_RATE, &actual_hz) != 0) {
        slog("g2d: V3D clock setup failed\r\n");
        return;
    }
    /* Firmware returning rate 0 leaves the V3D gated - force the
     * nominal BCM283x V3D rate so register accesses have a clock. */
    if (actual_hz == 0 && max_hz == 0) {
        if (g2d_clock_set(300000000u) == 0 &&
            g2d_clock_get(FW_GET_CLOCK_RATE, &actual_hz) == 0) {
            slog("g2d: V3D clock forced to %u Hz\r\n", actual_hz);
            return;
        }
        slog("g2d: V3D clock stuck at 0 Hz\r\n");
        return;
    }
    slog("g2d: V3D clock max=%u Hz actual=%u Hz\r\n", max_hz, actual_hz);
}

/* VC4 only: ask the firmware to enable user-space QPU access
 * (RPI_FIRMWARE_SET_ENABLE_QPU).  GPU_FFT issues this before its
 * first SRQ launch; without it the QPU fetches may be blocked. */
static int g2d_qpu_enable(void)
{
    g2d_qpu_enable_req_t *req;
    ewokos_addr_t vaddr;
    ewokos_addr_t phys;
    mail_message_t msg;
    int result = -1;

    if (bcm283x_mailbox_init() == 0)
        return -1;
    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0)
        return -1;
    req = (g2d_qpu_enable_req_t *)(uintptr_t)vaddr;
    memset(req, 0, sizeof(*req));
    req->buf_size = sizeof(*req);
    req->tag.tag = FW_SET_ENABLE_QPU;
    req->tag.value_buf_size = 4;
    req->tag.value_len = 4;
    req->tag.enable = 1;

    phys = dma_phy_addr(0, vaddr);
    if (phys != 0 && (sizeof(phys) <= 4 || (phys >> 32) == 0)) {
        memset(&msg, 0, sizeof(msg));
        msg.data = (((uint32_t)phys | MAILBOX_VC_ALIAS_NONCACHED) >> 4);
        msg.channel = PROPERTY_CHANNEL;
        if (bcm283x_mailbox_call_timeout(&msg, 0) == 0 &&
            (req->code & FW_RESPONSE) != 0 &&
            (req->tag.value_len & FW_RESPONSE) != 0)
            result = 0;
    }
    dma_free(0, vaddr);
    if (result != 0)
        slog("g2d: firmware QPU enable failed\r\n");
    return result;
}

/* BCM2835/2837: the V3D GRAFX power domain is not guaranteed to be
 * on at boot - the Linux vc4 driver turns it on through the firmware
 * (RPI_FIRMWARE_SET_DOMAIN_STATE) before touching any V3D register,
 * treating every domain as off at boot.  Register reads of a powered
 * down domain return garbage, so IDENT0 never matched on real Pi3
 * hardware until this call was added.  Old firmware without the
 * domains interface gets the legacy SET_POWER_STATE fallback (device
 * id 10, RPI_OLD_POWER_DOMAIN_V3D), the same fallback Linux uses for
 * USB. */
static int g2d_power_domain_call(uint32_t tag, uint32_t domain_id)
{
    g2d_domain_req_t *req;
    ewokos_addr_t vaddr;
    ewokos_addr_t phys;
    mail_message_t msg;
    int result = -1;

    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0)
        return -1;
    req = (g2d_domain_req_t *)(uintptr_t)vaddr;
    memset(req, 0, sizeof(*req));
    req->buf_size = sizeof(*req);
    req->tag.tag = tag;
    req->tag.value_buf_size = 8;
    req->tag.value_len = 8;
    req->tag.domain = domain_id;
    req->tag.on = 1;

    phys = dma_phy_addr(0, vaddr);
    if (phys != 0 && (sizeof(phys) <= 4 || (phys >> 32) == 0)) {
        memset(&msg, 0, sizeof(msg));
        msg.data = (((uint32_t)phys | MAILBOX_VC_ALIAS_NONCACHED) >> 4);
        msg.channel = PROPERTY_CHANNEL;
        if (bcm283x_mailbox_call_timeout(&msg, 0) == 0 &&
            (req->code & FW_RESPONSE) != 0 &&
            (req->tag.value_len & FW_RESPONSE) != 0)
            result = 0;
    }
    dma_free(0, vaddr);
    return result;
}

static void g2d_power_on_v3d(void)
{
    if (bcm283x_mailbox_init() == 0)
        return;
    if (g2d_power_domain_call(FW_SET_DOMAIN_STATE, FW_DOMAIN_V3D) == 0)
        return;
    if (g2d_power_domain_call(FW_SET_POWER_STATE, FW_OLD_DEVICE_V3D) == 0)
        return;
    slog("g2d: V3D power domain on failed\r\n");
}

/* ------------------------------------------------------------------ */
/* bring-up                                                            */
/* ------------------------------------------------------------------ */

/* Re-enable the V3D L2 cache (a reset may leave it disabled). */
static void g2d_l2c_enable(void)
{
    v3d_ctl()[CTL_L2CACTL / 4] = (1u << 2) | (1u << 0);    /* L2CCLR | L2CENA */
    g2d_dsb();
}

/* Write back dirty V3D caches to DRAM: flush the TMU write combiner,
 * then the L2T in CLEAN mode. */
static void g2d_flush_l2(void)
{
    uint32_t i;

    v3d_ctl()[CTL_L2TCACTL / 4] = (1u << 8);               /* TMUWCF */
    for (i = 0; i < 2000000 && (v3d_ctl()[CTL_L2TCACTL / 4] & (1u << 8)); i++) {}
    v3d_ctl()[CTL_L2TCACTL / 4] = (1u << 0) | (2u << 1);   /* L2TFLS | CLEAN */
    for (i = 0; i < 2000000 && (v3d_ctl()[CTL_L2TCACTL / 4] & (1u << 0)); i++) {}
    g2d_dsb();
}

/* Per-job GPU cache maintenance, exactly GPU_FFT's launch preamble
 * (proven on this V3D generation): clear the L2 and every slice
 * cache.  The staging addresses themselves carry the 0xC0000000
 * direct alias, so the fetches bypass the caches anyway - the clear
 * only protects against leftovers from any earlier cached access. */
static void g2d_invalidate_caches(void)
{
    v3d_ctl()[CTL_L2CACTL / 4] = 1u << 2;     /* L2 clear (GPU_FFT) */
    v3d_ctl()[CTL_SLCACTL / 4] = ~0u;         /* clear T1/T0/U/I slice caches */
    g2d_dsb();
}

/* V3D identification: returns the architecture version x10 (42, 21,
 * ...) or 0 when no V3D block answers at the expected offsets. */
static int g2d_probe(void)
{
    uint32_t id0, id1;

    id0 = v3d_hub()[HUB_IDENT0 / 4];
    if (id0 == HUB_IDENT0_EXPECT) {                     /* "VHUB" */
        _has_hub = 1;
        id1 = v3d_hub()[HUB_IDENT1 / 4];
        /* Linux drm/v3d: ver = TVER * 10 + REV */
        _ver = (int)(id1 & 0xFu) * 10 + (int)((id1 >> 4) & 0xFu);
        return (_ver >= 20) ? 1 : 0;
    }

    /* no hub: V3D 2.x single-core block (Pi3 BCM2837).  The core
     * registers sit at base+0 here - read IDENT0 at offset 0x00. */
    _has_hub = 0;
    id0 = v3d_ctl()[CTL_IDENT0 / 4];
    if ((id0 & 0x0FFFFFF0u) != 0x02443350u)
        return 0;
    _ver = 21;
    return 1;
}

/* QPU count: (QUPS+1) slices x (NSLC+1) QPUs per slice, from the core
 * IDENT1; falls back to 12 (the BCM2711/BCM2837 part count). */
static int g2d_count_qpus(void)
{
    uint32_t id1 = v3d_ctl()[CTL_IDENT1 / 4];
    int qups = (int)((id1 >> 8) & 0xFu) + 1;
    int nslc = (int)((id1 >> 4) & 0xFu) + 1;
    int n = qups * nslc;

    if (n <= 0 || n > 16)
        n = 12;
    return n;
}

/* Reset the V3D block via the PM power domain (assert/deassert
 * V3DRSTN).  On BCM2835/2711 the V3D domain is normally already
 * powered by the firmware, so the INIT-time power-cycle stays gated
 * by G2D_SKIP_PM_RESET; the function itself is always compiled
 * because it doubles as the wedge recovery (a hung SRQ thread can
 * only be reclaimed by resetting the domain - the raspi5 driver used
 * the same power-cycle to cure a "QPU array never launches" state).
 * Both callers sit behind debug switches, hence the unused marker. */
static void g2d_pm_reset(void) __attribute__((unused));
static void g2d_pm_reset(void)
{
    volatile uint32_t *pg = _pm + (PM_GRAFX_OFF / 4);
    uint32_t v = *pg;

    *pg = PM_PASSWORD | (v & ~PM_V3DRSTN);      /* assert V3D reset */
    g2d_dsb();
    usleep(20);
    v = *pg;
    *pg = PM_PASSWORD | (v & ~PM_V3DRSTN) | PM_V3DRSTN;
    g2d_dsb();
    usleep(200);
}

int v3d_g2d_init(void)
{
    sys_info_t si;
    uint32_t i;
    unsigned kn_total = 0;

    if (_inited)
        return _ok ? 0 : -1;
    _inited = 1;

    /* capture the physical RAM ranges the GPU may legally touch: the
     * allocable region, the sys_dma window and the IPC_CONTIG shm slab
     * (only the sub-4 GB part is usable by the 32-bit QPU addresses) */
    sys_get_sys_info(&si);
    _ram_alloc_base = si.allocable_phy_mem_base;
    _ram_alloc_top = si.allocable_phy_mem_top;
    _ram_dma_base = si.sys_dma.phy_base;
    _ram_dma_top = si.sys_dma.phy_base + si.sys_dma.size;
    _ram_contig_base = si.shm_contig.phy_base;
    _ram_contig_top = si.shm_contig.phy_base + si.shm_contig.size;
    _ram_total = si.total_phy_mem_size;

    /* the V3D and PM blocks sit INSIDE the 32 MB MMIO window that the
     * kernel maps into every process - no SYS_MEM_MAP needed */
    if (mmio_map() == 0)
        return -1;
    _v3d = (volatile uint32_t *)(uintptr_t)(_mmio_base + V3D_MMIO_OFF);
    _pm = (volatile uint32_t *)(uintptr_t)(_mmio_base + PM_MMIO_OFF);

    if (!g2d_probe()) {
        /* BCM2835/2837: the V3D GRAFX domain may be OFF at boot -
         * Linux powers it through the firmware before touching any
         * V3D register.  Power it on and probe again. */
        g2d_power_on_v3d();
        if (!g2d_probe()) {
            slog("g2d: no V3D block at mmio+0x%x (hub id0=0x%x id0=0x%x id1=0x%x)\r\n",
                 V3D_MMIO_OFF,
                 (uint32_t)v3d_hub()[HUB_IDENT0 / 4],
                 (uint32_t)v3d_ctl()[CTL_IDENT0 / 4],
                 (uint32_t)v3d_ctl()[CTL_IDENT1 / 4]);
            return -1;
        }
    }
    _num_qpus = g2d_count_qpus();
    slog("g2d: V3D %u.%u %s, %u QPUs\r\n",
         (uint32_t)(_ver / 10), (uint32_t)(_ver % 10),
         _has_hub ? "(hub)" : "(vc4 core)", (uint32_t)_num_qpus);

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
    _ksrc[KERN_FILL_VC4] = g2d_qpu_argb_fill_vc4; _ksrc_n[KERN_FILL_VC4] = g2d_qpu_argb_fill_vc4_n;
    _ksrc[KERN_BLIT_VC4] = g2d_qpu_argb_blit_vc4; _ksrc_n[KERN_BLIT_VC4] = g2d_qpu_argb_blit_vc4_n;
    _ksrc[KERN_ALPHA_VC4] = g2d_qpu_argb_alpha_vc4; _ksrc_n[KERN_ALPHA_VC4] = g2d_qpu_argb_alpha_vc4_n;
    _ksrc[KERN_ROTATE_VC4] = g2d_qpu_argb_rotate_vc4; _ksrc_n[KERN_ROTATE_VC4] = g2d_qpu_argb_rotate_vc4_n;
    for (i = 0; i < KERN_TOTAL; i++) {
        uint32_t k;
        _kcode[i] = (uint64_t *)(uintptr_t)dma_alloc(0, CSD_CODE_WORDS * 8);
        if (_kcode[i] == 0)
            return -1;
        for (k = 0; k < _ksrc_n[i]; k++)
            _kcode[i][k] = _ksrc[i][k];
        _kcode_p[i] = (uint32_t)dma_phy_addr(0, (ewokos_addr_t)(uintptr_t)_kcode[i]);
        if (_kcode_p[i] == 0)
            return -1;
        kn_total += _ksrc_n[i];
    }
    _unif = (uint32_t *)(uintptr_t)dma_alloc(0, CSD_UNIF_WORDS * 4);
    _scratch = (uint32_t *)(uintptr_t)dma_alloc(0, 4096u * 4u);
    if (_unif == 0 || _scratch == 0)
        return -1;
    _unif_p = (uint32_t)dma_phy_addr(0, (ewokos_addr_t)(uintptr_t)_unif);
    _scratch_p = (uint32_t)dma_phy_addr(0, (ewokos_addr_t)(uintptr_t)_scratch);
    if (_unif_p == 0 || _scratch_p == 0)
        return -1;
    /* nop kernel staging (VC4 bring-up diagnostics) */
    _nop_code = (uint64_t *)(uintptr_t)dma_alloc(0, 64u);
    if (_nop_code != 0) {
        memcpy(_nop_code, g2d_nop_vc4, sizeof(g2d_nop_vc4));
        _nop_code_p = (uint32_t)dma_phy_addr(0,
                        (ewokos_addr_t)(uintptr_t)_nop_code);
    }
    /* wedge-bisection diagnostic kernels (D1 uniform reads only,
     * D2 branch only, D3 host-interrupt exit, D4 fill front replica,
     * D5 minimal VPM write + VDW DMA, W1-W6 uniform/regfile/ifn data
     * path probes) */
    {
        static const struct { const uint64_t *src; unsigned n; } DIAG[22] = {
            { g2d_qpu_diag_unif_vc4,        g2d_qpu_diag_unif_vc4_n },
            { g2d_qpu_diag_branch_vc4,      g2d_qpu_diag_branch_vc4_n },
            { g2d_qpu_diag_host_vc4,        g2d_qpu_diag_host_vc4_n },
            { g2d_qpu_diag_fillfront_vc4,   g2d_qpu_diag_fillfront_vc4_n },
            { g2d_qpu_diag_vdw_vc4,         g2d_qpu_diag_vdw_vc4_n },
            { g2d_qpu_diag_unifacc_vc4,     g2d_qpu_diag_unifacc_vc4_n },
            { g2d_qpu_diag_rfacca_vc4,      g2d_qpu_diag_rfacca_vc4_n },
            { g2d_qpu_diag_rfaccb_vc4,      g2d_qpu_diag_rfaccb_vc4_n },
            { g2d_qpu_diag_unifadv_vc4,     g2d_qpu_diag_unifadv_vc4_n },
            { g2d_qpu_diag_unifrf_vc4,      g2d_qpu_diag_unifrf_vc4_n },
            { g2d_qpu_diag_rowsrf_vc4,      g2d_qpu_diag_rowsrf_vc4_n },
            { g2d_qpu_diag_ifn_vc4,         g2d_qpu_diag_ifn_vc4_n },
            { g2d_qpu_diag_rt_vc4,          g2d_qpu_diag_rt_vc4_n },
            { g2d_qpu_diag_rtb_vc4,         g2d_qpu_diag_rtb_vc4_n },
            { g2d_qpu_diag_rtd_vc4,         g2d_qpu_diag_rtd_vc4_n },
            { g2d_qpu_diag_rtn_vc4,         g2d_qpu_diag_rtn_vc4_n },
            { g2d_qpu_diag_rtbn_vc4,        g2d_qpu_diag_rtbn_vc4_n },
            { g2d_qpu_diag_rtg_vc4,         g2d_qpu_diag_rtg_vc4_n },
            { g2d_qpu_diag_rt2_vc4,         g2d_qpu_diag_rt2_vc4_n },
            { 0, 0 },
        };
        for (i = 0; i < 22; i++) {
            if (DIAG[i].src == 0)
                continue;
            _diag_code[i] = (uint64_t *)(uintptr_t)dma_alloc(
                                0, DIAG[i].n * 8u);
            if (_diag_code[i] != 0) {
                memcpy(_diag_code[i], DIAG[i].src, DIAG[i].n * 8u);
                _diag_code_p[i] = (uint32_t)dma_phy_addr(0,
                                (ewokos_addr_t)(uintptr_t)_diag_code[i]);
            }
        }
    }
#if G2D_HW_PROBE_ONLY
    /* TEMP-BISECT: stop here - no register writes.  The kernels sit in
     * staging but V3D is left exactly as the firmware booted it. */
    _ok = ((_ver >= 41 || _ver == 21) && kn_total > 0) ? 1 : 0;
    return 0;
#endif
#if !G2D_SKIP_PM_RESET
    g2d_pm_reset();      /* power-cycle GRAFX_V3D */
#endif
    g2d_l2c_enable();
    /* NOTE: no V3D MMU page table - the proven path runs without it */

    /* VC4: user-space QPU access needs a firmware handshake before the
     * first SRQ launch (GPU_FFT does the same at alloc time). */
    if (_ver == 21) {
        g2d_qpu_enable();
        /* Reset reserves NO VPM for user (SRQ) programs, and QPU
         * writes outside the reserved window are masked (3D guide:
         * "a portion of the VPM must be reserved for general-purpose
         * use").  Reserve the max 31 x 256B = 7936 bytes - legal
         * while idle, before any shading has commenced. */
        v3d_ctl()[V3D_VPMBASE / 4] = 0x1fu;
        g2d_dsb();
    }

#if G2D_VC4_NOP_TEST
    if (_ver == 21 && _nop_code_p != 0) {
        /* baseline: ERRSTAT is a sticky latch (no public bit doc) -
         * record it before the first launch so a later 0x50-like value
         * can be attributed to the launch that latched it */
        slog("g2d: VC4 ERRSTAT baseline=0x%x\r\n",
             v3d_ctl()[V3D_ERRSTAT / 4]);
        /* BRING-UP probe: launch the nop kernel immediately so the
         * boot log shows whether SRQ dispatch works on this silicon
         * independently of the real kernels.  The capture variant
         * records SRQCS at four points (before clear / after clear /
         * after the last enqueue / final poll value) to nail the
         * field semantics on this silicon. */
        uint32_t cap[4] = { 0 };
        g2d_invalidate_caches();
        if (g2d_vc4_launch_capture(_nop_code_p, _unif_p,
                                   (uint32_t)_num_qpus, cap) != 0) {
            v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
            slog("g2d: VC4 init nop launch FAILED SRQCS=0x%x\r\n", cap[3]);
        } else {
            slog("g2d: VC4 init nop launch ok\r\n");
        }
        slog("g2d: VC4 SRQCS seq pre=0x%x clr=0x%x enq=0x%x end=0x%x\r\n",
             cap[0], cap[1], cap[2], cap[3]);
    }

    if (_ver == 21 && _diag_code_p[4] != 0) {
        /* S probes: the minimal VPM+VDW store (diag_vdw) into the
         * scratch dma buffer, verified by the CPU AFTERWARD - the
         * first probes that observe a GPU WRITE (the D/W rounds could
         * only encode their answer in completion).  Production ops
         * all complete but land zero pixels, so the store path is the
         * open question.  S1 stores through the 0xC0000000 direct
         * alias (what bsp_g2d passes), S2 through the plain physical
         * address - a difference isolates alias handling in the VDW
         * address path.  Sentinel words separate "never written" from
         * "written with the wrong value". */
        static const uint32_t SENT = 0xA5A5A5A5u;
        uint32_t srqcs = 0, q, run;

        for (run = 0; run < 2; run++) {
            uint32_t dst_p = _scratch_p + run * 64u;   /* disjoint 16-word slots */
            uint32_t *dst_va = _scratch + run * 16u;

            for (q = 0; q < 16; q++)
                dst_va[q] = SENT;
            g2d_dsb();
            for (q = 0; q < 16; q++) {
                uint32_t *s = _unif + q * VC4_UNIF_QWORDS;

                s[0] = 0x12345678u;                     /* color */
                s[1] = 0;                               /* qid */
                s[2] = 0;                               /* L-1 */
                s[3] = run ? dst_p : (dst_p | V3D_VC_ALIAS_DIRECT);
                s[4] = 0;                               /* x0 */
                s[5] = 16;                              /* x1: one full group */
                s[6] = 0;                               /* rowjump */
                s[7] = 1;                               /* rows_q */
                s[8] = 0;                               /* gx0 */
            }
            g2d_invalidate_caches();
            if (g2d_vc4_launch(_diag_code_p[4], _unif_p, 1u, &srqcs) != 0) {
                g2d_vc4_dump_regs(run ? "S2" : "S1");
                slog("g2d: VC4 probe %s TIMEOUT SRQCS=0x%x\r\n",
                     run ? "S2" : "S1", srqcs);
                v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                (void)g2d_vc4_recover();
            } else {
                /* VDW stores pass through the V3D L2; without a
                 * CLEAN the lines can stay in the cache and the CPU
                 * readback sees the sentinel even though the DMA
                 * ran clean (exactly the 0x1000-ERRSTAT symptom).
                 * Read with and without the flush to discriminate. */
                uint32_t r0 = dst_va[0], r15 = dst_va[15];

                g2d_flush_l2();
                g2d_invalidate_caches();
                slog("g2d: VC4 probe %s pre w[0]=0x%x w[15]=0x%x\r\n",
                     run ? "S2" : "S1", r0, r15);
                slog("g2d: VC4 probe %s done w[0..3]=0x%x 0x%x 0x%x 0x%x "
                     "w[15]=0x%x err=0x%x\r\n",
                     run ? "S2" : "S1",
                     dst_va[0], dst_va[1], dst_va[2], dst_va[3],
                     dst_va[15], v3d_ctl()[V3D_ERRSTAT / 4]);
            }
        }

        /* RT probes: bisect the silent store.  The S probes now run
         * clean (no ERRSTAT bits) yet write nothing, so the setup
         * words are accepted but the data never reaches DRAM.  RT is
         * a pure QPU->VPM->QPU round trip (the guide guarantees read
         * data even outside the reserved window): it completes only
         * when BOTH the VPM write and the read work.  RTB controls
         * the read path alone.  RTD repeats the S store but polls
         * VPM_ST_BUSY to zero before exiting - a hang IS a result
         * (DMA started, never finished); completing with pixels
         * still missing means busy read 0 from the outset (the DMA
         * never started at all).
         * Round 5: the RT/RTB compares were themselves buggy (the 2nd
         * rf operand reads register file B, never written - a
         * guaranteed wedge), so both Round-4 TIMEOUTs were false
         * signals.  The compares now stage through an accumulator, and
         * RTN/RTBN are INVERSE-polarity twins (wedge on EQUAL):
         * exactly one of each pair must complete, so two TIMEOUTs
         * prove a true stall.  RTG replays the GPU_FFT store shape
         * verbatim (vertical VPM writes + UNITS=16 VDW) to separate
         * "our store shape is wrong" from "VDW is dead here".
         * Round 6 results: RTG DID store (row 0 landed) but the RT
         * family timed out on ANOTHER probe bug - the read setup word
         * 0x101A00 carries NUM=1 (Table 33 bits 23:20) while each
         * probe makes four raddr-48 reads, so every read past the
         * first never got data.  NUM is now 0 (=>16) and the latency
         * gap is plain nops (nop_reads were extra VPM reads!).  RTG
         * now writes DISTINCT colors (column i = color+i) and its
         * readback scans all 256 words: Table 35 defines the VDW
         * STRIDE as the GAP between rows, so 0xc0000040 may be a
         * 128B row pitch (rows at w0/w32/w64...) - Round 5 only
         * looked at w[0]/w[15]/w[16]/w[255].  RT2 = RTG with UNITS=1
         * bisects RTD's zero-byte UNITS=1 word. */
        {
            static const struct { const char *tag; int idx; } RTP[7] = {
                { "RT", 12 }, { "RTB", 13 }, { "RTD", 14 },
                { "RTN", 15 }, { "RTBN", 16 }, { "RTG", 17 },
                { "RT2", 18 },
            };
            uint32_t rt, rp;

            for (rt = 0; rt < 7; rt++) {
                uint32_t code_p = _diag_code_p[RTP[rt].idx];
                uint32_t front = (RTP[rt].idx == 14 || RTP[rt].idx == 17
                                  || RTP[rt].idx == 18);
                uint32_t *dst_va = _scratch +
                    (RTP[rt].idx == 17 ? 256
                     : RTP[rt].idx == 18 ? 512 : 32); /* RTD/RTG/RT2 */

                if (code_p == 0)
                    continue;
                if (front) {                  /* RTD/RTG/RT2: fill front */
                    for (rp = 0; rp < (RTP[rt].idx >= 17 ? 256u : 16u); rp++)
                        dst_va[rp] = SENT;
                    g2d_dsb();
                }
                for (rp = 0; rp < 16; rp++) {
                    uint32_t *s = _unif + rp * VC4_UNIF_QWORDS;

                    s[0] = 0x12345678u;             /* color */
                    s[1] = 0;                       /* qid */
                    s[2] = 0;                       /* L-1 */
                    s[3] = RTP[rt].idx == 14
                        ? (_scratch_p + 128u) | V3D_VC_ALIAS_DIRECT
                        : RTP[rt].idx == 17
                        ? (_scratch_p + 1024u) | V3D_VC_ALIAS_DIRECT
                        : RTP[rt].idx == 18
                        ? (_scratch_p + 2048u) | V3D_VC_ALIAS_DIRECT : 0;
                    s[4] = 0;                       /* x0 */
                    s[5] = 16;                      /* x1: one group */
                    s[6] = 0;                       /* rowjump */
                    s[7] = 1;                       /* rows_q */
                    s[8] = 0;                       /* gx0 */
                }
                g2d_invalidate_caches();
                if (g2d_vc4_launch(code_p, _unif_p, 1u, &srqcs) != 0) {
                    g2d_vc4_dump_regs(RTP[rt].tag);
                    slog("g2d: VC4 probe %s TIMEOUT SRQCS=0x%x\r\n",
                         RTP[rt].tag, srqcs);
                    v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                    (void)g2d_vc4_recover();
                } else if (front) {
                    g2d_flush_l2();
                    g2d_invalidate_caches();
                    if (RTP[rt].idx >= 17) {
                        /* RTG/RT2: full-buffer scan - every VPM column
                         * got a DISTINCT color, so the runs say which
                         * words landed where (row count AND pitch). */
                        uint32_t rs[8], rl[8], rn = 0, tot = 0, j;

                        for (rp = 0; rp < 256; rp++) {
                            if (dst_va[rp] == SENT)
                                continue;
                            tot++;
                            if (rp == 0 || dst_va[rp - 1] == SENT) {
                                if (rn < 8) { rs[rn] = rp; rl[rn] = 1; rn++; }
                            } else if (rn > 0) {
                                rl[rn - 1]++;
                            }
                        }
                        slog("g2d: VC4 probe %s done runs=%d tot=%d "
                             "err=0x%x\r\n", RTP[rt].tag, rn, tot,
                             v3d_ctl()[V3D_ERRSTAT / 4]);
                        for (j = 0; j < rn && j < 4; j++)
                            slog("g2d: VC4 probe %s run%d w%d..%d "
                                 "v0=0x%x vL=0x%x\r\n", RTP[rt].tag, j,
                                 rs[j], rs[j] + rl[j] - 1, dst_va[rs[j]],
                                 dst_va[rs[j] + rl[j] - 1]);
                    } else
                        slog("g2d: VC4 probe RTD done w[0..3]=0x%x 0x%x 0x%x "
                             "0x%x w[15]=0x%x err=0x%x\r\n",
                             dst_va[0], dst_va[1], dst_va[2], dst_va[3],
                             dst_va[15], v3d_ctl()[V3D_ERRSTAT / 4]);
                } else {
                    slog("g2d: VC4 probe %s ok\r\n", RTP[rt].tag);
                }
            }
        }
    }

#if G2D_VC4_PROBES
    if (_ver == 21) {
        /* Staged bring-up probes that encode their answer in the
         * COMPLETION itself (scratch/uniform reads never proved a GPU
         * write yet, so "ok" with silent memory stays ambiguous):
         *   D2  control: acc-setf + taken branch (was ok, must stay)
         *   W1  uniform word 0 -> ACCUMULATOR compare (no regfile)
         *   W2  loadimm -> rf20 (file A) -> acc round trip
         *   W2b ra20+ra21 written, compare reads rf20 via raddr_b
         *       (FILE B, never written): TIMEOUT is the EXPECTED
         *       result (split regfile); completion == the files alias
         *   W3  uniform stream advance: the 8th word must be rows_q
         *   W4  the fill 9-word uniform front, rf0 (color) readback
         *   W5  the fill front, rf10 (rows_q) readback
         *   W6  ifn conditional execution (N set must fire, N clear
         *       must not)
         * Probes launch ONE thread (nq=1): a hung probe then occupies
         * a single QPU and the following probes still run on the
         * other eleven (a 12-thread launch would wedge every QPU and
         * poison the rest of the sequence). */
        static const struct {
            const char *tag; int kern; uint32_t rows;
        } PRB[8] = {
            { "D2",  -2, 0 },   /* control: must stay ok */
            { "W1",  -6, 0 },   /* uniform word 0 -> acc */
            { "W2",  -7, 0 },   /* rf file-A round trip */
            { "W2b", -8, 0 },   /* file-B read: TIMEOUT expected */
            { "W3",  -9, 5 },   /* stream advance: u7 == rows_q */
            { "W4", -10, 0 },   /* fill front: rf0 == color */
            { "W5", -11, 1 },   /* fill front: rf10 == rows_q */
            { "W6", -12, 0 },   /* ifn conditional execution */
        };
        uint32_t prb, q, srqcs = 0;

        slog("g2d: VC4 probe addrs unif_p=0x%x fill_p=0x%x nop_p=0x%x "
             "scratch_p=0x%x w1=0x%x w2=0x%x w2b=0x%x w6=0x%x\r\n",
             _unif_p, _kcode_p[KERN_FILL_VC4], _nop_code_p, _scratch_p,
             _diag_code_p[5], _diag_code_p[6], _diag_code_p[7],
             _diag_code_p[11]);

        for (prb = 0; prb < 8; prb++) {
            uint32_t code_p = (PRB[prb].kern < 0)
                ? _diag_code_p[-PRB[prb].kern - 1]
                : _kcode_p[PRB[prb].kern];
            if (code_p == 0)
                continue;
            for (q = 0; q < 16; q++) {
                uint32_t *s = _unif + q * VC4_UNIF_QWORDS;

                s[0] = 0x12345678u;             /* color */
                s[1] = q;                       /* qid */
                s[2] = 0;                       /* L-1 */
                s[3] = 0;                       /* dst row 0 */
                s[4] = 0;                       /* x0 */
                s[5] = 0;                       /* x1 */
                s[6] = 0;                       /* rowjump */
                s[7] = PRB[prb].rows;           /* rows_q */
                s[8] = 0;                       /* gx0 */
            }
            g2d_invalidate_caches();
            /* one thread: a hang costs one QPU, not the whole array */
            if (g2d_vc4_launch(code_p, _unif_p, 1u, &srqcs) != 0) {
                g2d_vc4_dump_regs(PRB[prb].tag);
                slog("g2d: VC4 probe %s TIMEOUT SRQCS=0x%x\r\n",
                     PRB[prb].tag, srqcs);
                (void)g2d_vc4_recover();
            } else {
                slog("g2d: VC4 probe %s ok\r\n", PRB[prb].tag);
            }
        }
    }
#endif
#endif

    /* usable when the dispatch engine and the matching kernel flavor
     * are available: CSD + CSD kernels on V3D >= 4.1, the SRQ
     * launcher + *_vc4 kernels on V3D 2.1. */
    _ok = ((_ver >= 41 || _ver == 21) && kn_total > 0) ? 1 : 0;
    if (!_ok)
        slog("g2d: GPU not dispatchable (ver=%u kernels=%u)\r\n",
             (uint32_t)_ver, kn_total);
    return 0;
}

int v3d_g2d_ready(void)
{
    return _ok;
}

int v3d_g2d_ver(void)
{
    return _ver;
}

int v3d_g2d_num_qpus(void)
{
    return (_num_qpus > 0) ? _num_qpus : 12;
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

/* ------------------------------------------------------------------ */
/* CSD dispatch                                                        */
/* ------------------------------------------------------------------ */

int v3d_g2d_run(const uint64_t *code, int nwords,
                const uint32_t *unifs, int nunifs,
                int num_qpus,
                const void *src, size_t src_len,
                void *dst, size_t dst_len)
{
    uint32_t csd_base = (_ver >= 71) ? CSD_CFG_BASE_V7 : CSD_CFG_BASE_OLD;
    uint32_t csd_done = (_ver >= 71) ? INT_CSDDONE_V7 : INT_CSDDONE_OLD;
    volatile uint32_t *csd =
        _v3d + (((_has_hub ? V3D_CORE0_OFF : 0u) + csd_base) / 4);
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
    else if (code == g2d_qpu_argb_rotate)
        kern = KERN_ROTATE;
    else
        return -1;      /* only the four bsp_g2d kernels are supported */
    if ((uint32_t)nwords > _ksrc_n[kern])
        return -1;

    /* make the caller's ARM-side writes visible to the GPU, and drop the
     * ARM's stale copies of the destination.  NOCACHE dma/contig
     * canvases need no maintenance beyond the dsb inside the helpers. */
    if (src && src_len && !is_dma_addr(src))
        g2d_dsb();
    if (dst && dst_len && !is_dma_addr(dst))
        g2d_dsb();

    /* only the uniforms change per call; the kernel is already in dma */
    for (i = 0; i < (uint32_t)nunifs; i++)
        _unif[i] = unifs[i];
    /* extra trailing uniform: scratch base for the kernels' flush
     * epilogue (physical address - the QPU has no MMU) */
    _unif[nunifs] = _scratch_p;
    g2d_invalidate_caches();

    /* V3D 7.1: the exact config of the proven raspi5 path.  V3D 4.2:
     * Mesa's compute model (drm/v3d CSD submit) - num_qpus workgroups
     * of 16 threads each (one QPU per workgroup, four threads of four
     * elements), all in a single supergroup, NUM_BATCHES counted minus
     * one, CFG5 carries the VC4 shader-record bits.  Writing CFG0 last
     * starts the dispatch (Linux drm/v3d order). */
    if (_ver >= 71) {
        cfg[0] = 1u << 16;                  /* NUM_WGS_X = 1 */
        cfg[3] = 0x000FF010u;
        cfg[4] = (uint32_t)num_qpus;
        cfg[5] = _kcode_p[kern];            /* preloaded kernel, physical */
    } else {
        uint32_t nq = (uint32_t)num_qpus;

        if (nq > 16)
            return -1;                      /* WGS_PER_SG is 4 bits */
        cfg[0] = nq << 16;                  /* workgroup count X */
        cfg[1] = 1u << 16;                  /* workgroup count Y */
        cfg[2] = 1u << 16;                  /* workgroup count Z */
        cfg[3] = ((nq & 0xfu) << 8)         /* WGS_PER_SG (0 encodes 16) */
               | ((nq - 1u) << 12)          /* BATCHES_PER_SG - 1 */
               | 16u;                       /* WG_SIZE */
        cfg[4] = nq - 1u;                   /* NUM_BATCHES - 1 */
        cfg[5] = _kcode_p[kern] | CSD_CFG5_THREADING
               | CSD_CFG5_SINGLE_SEG | CSD_CFG5_PROP_NANS;
    }
    cfg[6] = _unif_p;
    cfg[7] = 0;                         /* V3D 7.x CFG7 only */
    for (i = 1; i <= (uint32_t)((_ver >= 71) ? 7 : 6); i++)
        csd[i] = cfg[i];
    csd[0] = cfg[0];                    /* sole CFG0 write starts it */

    /* Every production kernel waits for pending TMU writes and then
     * uses the legal thread-end protocol, so CSDDONE is authoritative. */
    for (i = 0; i < 2000000; i++)
        if (v3d_ctl()[INT_STS / 4] & csd_done)
            break;
    if (i == 2000000) {
        v3d_ctl()[INT_CLR / 4] = csd_done;
        /* A timeout is a real failure.  Do not reset the graphics domain
         * and do not replay this possibly-live operation. */
        return 1;
    }
    v3d_ctl()[INT_CLR / 4] = csd_done;

    /* GPU writes -> DRAM, then drop the ARM's stale destination lines */
    g2d_flush_l2();
    if (dst && dst_len && !is_dma_addr(dst))
        g2d_dsb();
    return 0;
}

/* ------------------------------------------------------------------ */
/* VC4 (V3D 2.1) SRQ dispatch                                          */
/* ------------------------------------------------------------------ */

/* GPU_FFT launch sequence: disable the doorbell machinery, clear the
 * QPU interrupt throttles, then enqueue one thread per QPU.  Each
 * SRQPC write enqueues one thread AND snapshots the current SRQUA, so
 * the uniform address must advance per enqueue (one 32-word slot per
 * QPU, GPU_FFT does the same) - with a shared SRQUA every QPU would
 * read slot 0's words.  Addresses carry the 0xC0000000 direct VC bus
 * alias - exactly what GPU_FFT hands SRQUA/SRQPC on Pi2/3 (mem_alloc
 * flag 0x4 = MEM_FLAG_COHERENT).  The caller must have run
 * g2d_invalidate_caches first so the freshly
 * written staging is fetched from DRAM.  On timeout SRQCS is left
 * UNCLEANED and returned through `srqcs_out` for diagnostics; the
 * caller must clear it.  Returns 0 on completion, 1 on timeout. */
static int g2d_vc4_launch(uint32_t code_p, uint32_t unif_p, uint32_t nq,
                          uint32_t *srqcs_out)
{
    volatile uint32_t *core = v3d_ctl();
    uint32_t i, q;

    core[V3D_DBCFG / 4] = 0;
    core[V3D_DBQITE / 4] = 0;
    core[V3D_DBQITC / 4] = ~0u;
    core[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
    for (q = 0; q < nq; q++) {
        core[V3D_SRQUA / 4] = (unif_p + q * (VC4_UNIF_QWORDS * 4u))
                              | V3D_VC_ALIAS_DIRECT;
        core[V3D_SRQPC / 4] = code_p | V3D_VC_ALIAS_DIRECT;
    }

    /* SRQCS bits 23:16 count completed threads */
    for (i = 0; i < 2000000; i++)
        if (((core[V3D_SRQCS / 4] >> 16) & 0xFFu) == nq)
            break;
    if (i == 2000000) {
        if (srqcs_out)
            *srqcs_out = core[V3D_SRQCS / 4];
        return 1;
    }
    core[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
    return 0;
}

/* Init-time diagnostic variant of g2d_vc4_launch: captures SRQCS at
 * four points (before clear / after clear / after the last enqueue /
 * final poll value) on a KNOWN-GOOD launch so the field semantics can
 * be checked against this silicon.  Leaves SRQCS uncleared on timeout
 * (the caller clears it). */
static int g2d_vc4_launch_capture(uint32_t code_p, uint32_t unif_p,
                                  uint32_t nq, uint32_t cap[4])
{
    volatile uint32_t *core = v3d_ctl();
    uint32_t i, q;

    cap[0] = core[V3D_SRQCS / 4];
    core[V3D_DBCFG / 4] = 0;
    core[V3D_DBQITE / 4] = 0;
    core[V3D_DBQITC / 4] = ~0u;
    core[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
    cap[1] = core[V3D_SRQCS / 4];
    for (q = 0; q < nq; q++) {
        core[V3D_SRQUA / 4] = (unif_p + q * (VC4_UNIF_QWORDS * 4u))
                              | V3D_VC_ALIAS_DIRECT;
        core[V3D_SRQPC / 4] = code_p | V3D_VC_ALIAS_DIRECT;
    }
    cap[2] = core[V3D_SRQCS / 4];
    for (i = 0; i < 2000000; i++)
        if (((core[V3D_SRQCS / 4] >> 16) & 0xFFu) == nq)
            break;
    cap[3] = core[V3D_SRQCS / 4];
    if (i == 2000000)
        return 1;
    core[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
    return 0;
}

/* Dump the VC4 state around a wedged launch BEFORE any register is
 * cleaned (SRQCS first so the captured done/req/err fields survive). */
static void g2d_vc4_dump_regs(const char *tag)
{
    volatile uint32_t *core = v3d_ctl();

    slog("g2d: VC4 %s regs SRQCS=0x%x SQRSV1=0x%x SQCNTL=0x%x ERRSTAT=0x%x\r\n",
         tag,
         (uint32_t)core[V3D_SRQCS / 4],
         (uint32_t)core[V3D_SQRSV1 / 4],
         (uint32_t)core[V3D_SQCNTL / 4],
         (uint32_t)core[V3D_ERRSTAT / 4]);
}

/* Wedge recovery (raspi5 pattern): a hung SRQ thread can only be
 * reclaimed by power-cycling the GRAFX.V3D domain.  Clear the SRQ
 * state, assert/deassert V3DRSTN, re-enable L2 and the caches, then
 * prove dispatch works again with a single-QPU nop.  Returns 0 when
 * the nop completes. */
static int g2d_vc4_recover(void)
{
#if !G2D_PM_RESET_RECOVERY
    /* the PM write is suspected of freezing the box (see the switch
     * comment) - stay read-only: log the wedge and leave it in place */
    slog("g2d: VC4 wedge recovery skipped (PM reset gated)\r\n");
    return 1;
#else
    uint32_t srqcs = 0;

    if (_nop_code_p == 0)
        return 1;
    v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
    g2d_pm_reset();
    g2d_l2c_enable();
    g2d_invalidate_caches();
    if (g2d_vc4_launch(_nop_code_p, _unif_p, 1, &srqcs) != 0) {
        v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
        slog("g2d: VC4 pm-reset recovery FAILED (nop SRQCS=0x%x)\r\n",
             srqcs);
        return 1;
    }
    slog("g2d: VC4 pm-reset recovery ok\r\n");
    return 0;
#endif
}

/* Launch one SRQ thread per QPU of `code` (GPU_FFT protocol): stage
 * per-QPU uniforms + code address, then poll SRQCS until every thread
 * completed.  The uniform stream is per-QPU: QPU q reads its words at
 * _unif + q * VC4_UNIF_QWORDS. */
int v3d_g2d_run_vc4(const uint64_t *code, int nwords,
                    const uint32_t *unifs, int num_qpus,
                    const void *src, size_t src_len,
                    void *dst, size_t dst_len)
{
    volatile uint32_t *core = v3d_ctl();
    uint32_t nq = (uint32_t)num_qpus;
    uint32_t i, q, srqcs = 0;
    int kern = -1;

    if (code == NULL || nwords <= 0 || nwords > CSD_CODE_WORDS ||
        unifs == NULL || nq == 0 || nq > 16 || !_ok || _ver != 21)
        return -1;

    /* select the preloaded VC4 kernel staging */
    if (code == g2d_qpu_argb_fill_vc4)
        kern = KERN_FILL_VC4;
    else if (code == g2d_qpu_argb_blit_vc4)
        kern = KERN_BLIT_VC4;
    else if (code == g2d_qpu_argb_alpha_vc4)
        kern = KERN_ALPHA_VC4;
    else if (code == g2d_qpu_argb_rotate_vc4)
        kern = KERN_ROTATE_VC4;
    else
        return -1;      /* only the four bsp_g2d kernels are supported */
    if ((uint32_t)nwords > _ksrc_n[kern])
        return -1;

    /* make the caller's ARM-side writes visible to the GPU, and drop
     * the ARM's stale copies of the destination (same contract as the
     * CSD path). */
    if (src && src_len && !is_dma_addr(src))
        g2d_dsb();
    if (dst && dst_len && !is_dma_addr(dst))
        g2d_dsb();

    /* per-QPU uniform slots (32 words each; extra words ignored) */
    for (q = 0; q < nq; q++)
        for (i = 0; i < VC4_UNIF_QWORDS; i++)
            _unif[q * VC4_UNIF_QWORDS + i] =
                unifs[q * VC4_UNIF_QWORDS + i];

    /* the V3D caches do not snoop CPU writes: drop stale L2 + slice
     * lines so the freshly written uniforms and the preloaded kernel
     * are fetched from DRAM (vc4_flush_caches discipline) */
    g2d_invalidate_caches();

    if (g2d_vc4_launch(_kcode_p[kern], _unif_p, nq, &srqcs) == 0) {
        /* GPU writes -> DRAM, then drop the ARM's stale dst lines */
        g2d_flush_l2();
        if (dst && dst_len && !is_dma_addr(dst))
            g2d_dsb();
        return 0;
    }

    /* Timeout diagnostics: dump the VC4 state (SRQCS done 23:16,
     * req 15:8, err 7), clear it, then re-launch the nop kernel to
     * separate a dead launch path (nop also times out - power/clock/
     * reset/QPU lock) from a hung real kernel (nop completes).  When
     * the nop also dies the QPU array is wedged - run the PM_GRAFX
     * power-cycle recovery. */
    g2d_vc4_dump_regs("SRQ timeout");
    core[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
    slog("g2d: VC4 SRQ timeout kern=%u nq=%u SRQCS=0x%x\r\n",
         (uint32_t)kern, nq, srqcs);
    g2d_invalidate_caches();
    if (_nop_code_p != 0 &&
        g2d_vc4_launch(_nop_code_p, _unif_p, nq, &srqcs) != 0) {
        core[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
        slog("g2d: VC4 nop launch also timed out (SRQCS=0x%x) - "
             "QPU not dispatching\r\n", srqcs);
        (void)g2d_vc4_recover();
    } else if (_nop_code_p != 0) {
        slog("g2d: VC4 nop launch completed - kernel %u hangs\r\n",
             (uint32_t)kern);
    }
    /* A timeout is a real failure.  Do not replay this possibly-live
     * operation. */
    return 1;
}
