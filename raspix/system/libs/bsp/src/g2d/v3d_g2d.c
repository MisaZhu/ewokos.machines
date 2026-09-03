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
 *   - V3D 2.1 has no usable CSD: the *_vc4 kernels run through the
 *     SRQ user-program launcher (the GPU_FFT protocol).
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
/* Coherent alias: the one the bare-metal loader's mailbox proved against
 * this firmware on silicon (videocore app/src/mailbox.c BUS_ADDR) and
 * cpud's property fallback; see g2d_fw_mbox_call. */
#define MAILBOX_VC_ALIAS_COHERENT 0xC0000000u

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
#define V3D_SRQUL    0x438u          /* uniforms left (read) */
#define V3D_SRQCS    0x43cu          /* 23:16 done, 15:8 req, 7 err */
#define V3D_SQRSV0   0x410u          /* QPU scheduler reservations 0 */
#define V3D_SQRSV1   0x414u          /* QPU scheduler reservations 1 */
#define V3D_VPACNTL  0x500u          /* VPM allocator control */
#define V3D_VPMBASE  0x504u          /* user VPM reservation, 256B units */
/* GPU_FFT's reset value: clear the done counter (bit 16), the request
 * counter (bit 8) and the error flag (bit 7). */
#define V3D_SRQCS_CLEAR  ((1u << 16) | (1u << 8) | (1u << 7))
/* GPU_FFT also clears the L2 cache and the "other caches" (slice
 * caches) before every dispatch: without these clears a kernel whose
 * code/uniform staging was written through the ARM's non-cached
 * window can be fetched stale by the QPU, and a wedged uniform read
 * then never completes.  Pi3 bring-up fix (see g2d_vc4_launch). */
#define V3D_L2CACTL   0x20u
#define V3D_SLCACTL   0x24u

/* ---- PM power domain ---- */
#define PM_GRAFX_OFF 0x10cu
#define PM_PASSWORD  0x5A000000u
#define PM_V3DRSTN   (1u << 6)

#define CSD_CODE_WORDS 256
#define CSD_UNIF_WORDS 1024     /* _unif allocation, in words */

/* _unif is indexed as _unif[q * VC4_UNIF_QWORDS + i] for every q below the
 * maximum QPU count, so the allocation has to cover the widest per-QPU
 * contract at that count.  Guard the pairing rather than letting a future
 * VC4_UNIF_QWORDS increase silently run off the end of the buffer. */
#if 16 * VC4_UNIF_QWORDS > CSD_UNIF_WORDS
#error "CSD_UNIF_WORDS must hold 16 QPUs x VC4_UNIF_QWORDS words"
#endif
/* The per-QPU uniform stride must cover the widest VC4 contract in the
 * tree (the copy-loop kernel reads s[0..6]); keep headroom so a wider
 * future contract fails here instead of reading a neighbour's words. */
#if VC4_UNIF_QWORDS < 8
#error "VC4_UNIF_QWORDS must be at least the copy-loop contract's 7 words"
#endif

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
#define G2D_VC4_NOP_TEST 0

/* DEBUG SWITCH: Pi3 bring-up bisection - launch a battery of minimal
 * kernels (exit-only, 1 uniform read, 9 uniform reads) to isolate which
 * instruction stream wedges the SRQ dispatch.  0 for production. */
#define G2D_VC4_MICRO_TEST 0

/* Pi3 bring-up: GPU_FFT on Pi 2/3 feeds SRQPC the PLAIN (L2-cached)
 * VC address of its code - mem_alloc returns GPU-side addresses in
 * 0x00000000-0x3FFFFFFF and no 0x40000000 alias is added (that alias
 * is the Pi 1's SDRAM offset).  If the address bisect shows the
 * alias fetch wedging, set this to 1: SRQPC then carries the plain
 * address (SRQUA keeps the alias for the uniform data). */
#define G2D_VC4_PLAIN_SRQPC 0

/* Pi3 bring-up: run only the first SWEEP entry (single geometry per
 * boot, pristine V3D) instead of the whole table. */
#define G2D_VC4_SWEEP_ONLY_FIRST 1

/* Pi3 bring-up: alias-vs-plain address bisect of the fill kernel.
 * Each failing entry wedges the whole SRQ, so keep this OFF for
 * production/test runs (it is a debugging-only tool). */
#define G2D_VC4_BISECT 0

/* The staged fill/VDW bring-up battery is intentionally opt-in.  It is
 * useful while proving a new instruction sequence, but a wedged probe can
 * hold the VC4 SRQ for its full timeout and prevent a real API call from
 * ever running. */
#ifndef G2D_VC4_PROBES
#define G2D_VC4_PROBES 0
#endif

/* nop kernel: exits immediately - used to separate "QPU launch never
 * completes" from "the dispatched kernels hang". */
static const uint64_t g2d_nop_vc4[] = {
    0x100009e7009e7000ULL,  /* nop */
    0x100009e7009e7000ULL,  /* nop */
    0x300009e7009e7000ULL,  /* thread end */
    0x100009e7009e7000ULL,  /* nop */
    0x100009e7009e7000ULL,  /* nop */
};

/* Pi3 bring-up bisection kernels (assembled with the EwokOS qpuasm
 * K21 VC4 backend; see machines/raspix/.../g2d/tools/gen_kernels.py).
 *  - t_exit:   GPU_FFT completion write (mov host, r0; prog_end) only
 *  - t_unif1:  one uniform read then the same exit
 *  - t_unif9:  the argb_fill_vc4 uniform prologue then the exit        */
#if G2D_VC4_MICRO_TEST
static const uint64_t g2d_t_exit_vc4[] = {
    0x100009e7009e7000ULL,  /* nop */
    0x100009e7009e7000ULL,  /* nop */
    0xe002082700000001ULL,  /* mov r0, #1   ; exit flag */
    0x100209a7159e7000ULL,  /* mov interrupt, r0 */
    0x300009e7009e7000ULL,  /* nop; nop; thrend */
    0x100009e7009e7000ULL,  /* exit delay slots */
    0x100009e7009e7000ULL,  /* exit delay slots */
};
static const uint64_t g2d_t_unif1_vc4[] = {
    0x1002086715827d80ULL,  /* u0 -> r1 */
    0x100208a715827d80ULL,  /* u1 -> r2 */
    0xe002082700000001ULL,  /* mov r0, #1   ; exit flag */
    0x100209a7159e7000ULL,  /* mov interrupt, r0 */
    0x300009e7009e7000ULL,  /* nop; nop; thrend */
    0x100009e7009e7000ULL,  /* exit delay slots */
    0x100009e7009e7000ULL,  /* exit delay slots */
};
static const uint64_t g2d_t_unif9_vc4[] = {
    0x1002002715827d80ULL,  /* u0 -> rf0 */
    0x1002056715827d80ULL,  /* u1 -> rf21 */
    0x1002032715827d80ULL,  /* u2 -> rf12 */
    0x1002012715827d80ULL,  /* u3 -> rf4 */
    0x1002016715827d80ULL,  /* u4 -> rf5 */
    0x100201a715827d80ULL,  /* u5 -> rf6 */
    0x100201e715827d80ULL,  /* u6 -> rf7 */
    0x100202a715827d80ULL,  /* u7 -> rf10 */
    0x1002066715827d80ULL,  /* u8 -> rf25 */
    0xe002082700000001ULL,  /* mov r0, #1   ; exit flag */
    0x100209a7159e7000ULL,  /* mov interrupt, r0 */
    0x300009e7009e7000ULL,  /* nop; nop; thrend */
    0x100009e7009e7000ULL,  /* exit delay slots */
    0x100009e7009e7000ULL,  /* exit delay slots */
};
static const uint64_t g2d_t_branch_vc4[] = {
    0x100202a715827d80ULL,  /* u0 -> rf10 (rows) */
    0x1002002715827d80ULL,  /* u1 -> rf0 */
    0xd00228a70d280dc0ULL,  /* rows == 0 ? */
    0xf00809e700000010ULL,  /* brr.allz SKIP ; skip body */
    0x100009e7009e7000ULL,  /* branch delay slots */
    0x100009e7009e7000ULL,  /* branch delay slots */
    0x100009e7009e7000ULL,  /* branch delay slots */
    0x100009e7009e7000ULL,  /* body (never executed) */
    0x100009e7009e7000ULL,  /* body */
    0xe002082700000001ULL,  /* mov r0, #1   ; exit flag */
    0x100209a7159e7000ULL,  /* mov interrupt, r0 */
    0x300009e7009e7000ULL,  /* nop; nop; thrend */
    0x100009e7009e7000ULL,  /* exit delay slots */
    0x100009e7009e7000ULL,  /* exit delay slots */
};
static const uint64_t g2d_t_store_vc4[] = {
    0x1002012715827d80ULL,  /* u0 -> rf4 (dst row0) */
    0x1002086715827d80ULL,  /* u1 -> r1 (value) */
    0xe00208a700101a00ULL,  /* vpm_setup(1,1,h32(0)) */
    0x10020c67159e7480ULL,  /* VPM write setup */
    0x10020c27159e7240ULL,  /* 16 lanes -> VPM */
    0x100009e7159f2fc0ULL,  /* vw_wait */
    0xe00208a780804000ULL,  /* vdw base */
    0xd00208e7119d03c0ULL,  /* DEPTH = 16 */
    0x100208a7159e74c0ULL,  /* */
    0x10020c67159e7480ULL,  /* vw_setup basic */
    0xe0020c67c0000000ULL,  /* vw_setup stride */
    0x10020ca715127d80ULL,  /* vw_addr fires */
    0x100009e7159f2fc0ULL,  /* vw_wait */
    0xe002082700000001ULL,  /* mov r0, #1   ; exit flag */
    0x100209a7159e7000ULL,  /* mov interrupt, r0 */
    0x300009e7009e7000ULL,  /* nop; nop; thrend */
    0x100009e7009e7000ULL,  /* exit delay slots */
    0x100009e7009e7000ULL,  /* exit delay slots */
};
static const uint64_t g2d_t_tmuread_vc4[] = {
    0x1002096715827d80ULL,  /* u0 -> r5 (addr) */
    0x1002012715827d80ULL,  /* u1 -> rf4 (dst) */
    0xd0020e270c9c0bc0ULL,  /* tmu0 addr */
    0xa00009e7009e7000ULL,  /* ldtmu0 -> r4 */
    0x10020867159e7900ULL,  /* r1 = tmu word */
    0xe00208a700101a00ULL,  /* vpm_setup(1,1,h32(0)) */
    0x10020c67159e7480ULL,  /* VPM write setup */
    0x10020c27159e7240ULL,  /* 16 lanes -> VPM */
    0x100009e7159f2fc0ULL,  /* vw_wait */
    0xe00208a780804000ULL,  /* vdw base */
    0xd00208e7119d03c0ULL,  /* DEPTH = 16 */
    0x100208a7159e74c0ULL,  /* */
    0x10020c67159e7480ULL,  /* vw_setup basic */
    0xe0020c67c0000000ULL,  /* vw_setup stride */
    0x10020ca715127d80ULL,  /* vw_addr fires */
    0x100009e7159f2fc0ULL,  /* vw_wait */
    0xe002082700000001ULL,  /* mov r0, #1   ; exit flag */
    0x100209a7159e7000ULL,  /* mov interrupt, r0 */
    0x300009e7009e7000ULL,  /* nop; nop; thrend */
    0x100009e7009e7000ULL,  /* exit delay slots */
    0x100009e7009e7000ULL,  /* exit delay slots */
};
static const uint64_t g2d_t_bloop_vc4[] = {
    0x100202a715827d80ULL,  /* u0 -> rf10 (iters) */
    0xd00222a70d281dc0ULL,  /* LOOP: iters-- */
    0xd00228a70d280dc0ULL,  /* iters == 0 ? */
    0xf03809e7ffffffd0ULL,  /* brr.anynz LOOP ; while iters != 0 */
    0x100009e7009e7000ULL,  /* branch delay slots */
    0x100009e7009e7000ULL,  /* branch delay slots */
    0x100009e7009e7000ULL,  /* branch delay slots */
    0xe002082700000001ULL,  /* mov r0, #1   ; exit flag */
    0x100209a7159e7000ULL,  /* mov interrupt, r0 */
    0x300009e7009e7000ULL,  /* nop; nop; thrend */
    0x100009e7009e7000ULL,  /* exit delay slots */
    0x100009e7009e7000ULL,  /* exit delay slots */
};
static const uint64_t g2d_t_store2_vc4[] = {
    0x1002012715827d80ULL,  /* u0 -> rf4 (dst row0) */
    0x1002086715827d80ULL,  /* u1 -> r1 (value) */
    0xe00208a700101a00ULL,  /* vpm_setup(1,1,h32(0)) */
    0x10020c67159e7480ULL,  /* VPM write setup */
    0x10020c27159e7240ULL,  /* 16 lanes -> VPM */
    0x100009e7159f2fc0ULL,  /* vw_wait */
    0xe00208a780804000ULL,  /* vdw base */
    0xd00208e7119d03c0ULL,  /* DEPTH = 16 */
    0x100208a7159e74c0ULL,  /* */
    0x10020c67159e7480ULL,  /* vw_setup basic */
    0xe0020c67c0000000ULL,  /* vw_setup stride */
    0x10020ca715127d80ULL,  /* vw_addr fires */
    0x100009e7159f2fc0ULL,  /* vw_wait */
    0x10020c67159e7480ULL,  /* vw_setup basic (2nd) */
    0xe0020c67c0000000ULL,  /* vw_setup stride (2nd) */
    0xd00201270c112dc0ULL,  /* dst += 64 */
    0x10020ca715127d80ULL,  /* vw_addr fires (2nd) */
    0x100009e7159f2fc0ULL,  /* vw_wait */
    0xe002082700000001ULL,  /* mov r0, #1   ; exit flag */
    0x100209a7159e7000ULL,  /* mov interrupt, r0 */
    0x300009e7009e7000ULL,  /* nop; nop; thrend */
    0x100009e7009e7000ULL,  /* exit delay slots */
    0x100009e7009e7000ULL,  /* exit delay slots */
};
static const uint64_t g2d_t_store3_vc4[] = {
    0x1002012715827d80ULL,  /* u0 -> rf4 (dst) */
    0x1002086715827d80ULL,  /* u1 -> r1 (value) */
    0xe00208a700101a00ULL,  /* vpm_setup(1,1,h32(0)) */
    0x10020c67159e7480ULL,  /* VPM write setup */
    0x10020c27159e7240ULL,  /* 16 lanes -> VPM */
    0x100009e7159f2fc0ULL,  /* vw_wait */
    0xe00208a780804000ULL,  /* vdw base */
    0xd00208e7119d03c0ULL,  /* DEPTH = 16 */
    0x100208a7159e74c0ULL,  /* */
    0x10020c67159e7480ULL,  /* vw_setup basic */
    0xe0020c67c0000000ULL,  /* vw_setup stride */
    0x10020ca715127d80ULL,  /* vw_addr fires */
    0x100009e7159f2fc0ULL,  /* vw_wait */
    0xe00208a700101a00ULL,  /* vpm_setup(1,1,h32(0)) */
    0x10020c67159e7480ULL,  /* VPM write setup (2nd) */
    0x10020c27159e7240ULL,  /* 16 lanes -> VPM (2nd) */
    0x100009e7159f2fc0ULL,  /* vw_wait */
    0xe00208a780804000ULL,  /* vdw base (2nd) */
    0xd00208e7119d03c0ULL,  /* DEPTH = 16 */
    0x100208a7159e74c0ULL,  /* */
    0x10020c67159e7480ULL,  /* vw_setup basic (2nd) */
    0xe0020c67c0000000ULL,  /* vw_setup stride (2nd) */
    0xd00201270c112dc0ULL,  /* dst += 64 */
    0x10020ca715127d80ULL,  /* vw_addr fires (2nd) */
    0x100009e7159f2fc0ULL,  /* vw_wait */
    0xe002082700000001ULL,  /* mov r0, #1   ; exit flag */
    0x100209a7159e7000ULL,  /* mov interrupt, r0 */
    0x300009e7009e7000ULL,  /* nop; nop; thrend */
    0x100009e7009e7000ULL,  /* exit delay slots */
    0x100009e7009e7000ULL,  /* exit delay slots */
};
#endif

/* forward declaration (init-time nop probe uses it before the SRQ
 * dispatch section defines it) */
static int g2d_vc4_launch(uint32_t code_p, uint32_t unif_p, uint32_t nq,
                          uint32_t *srqcs_out);
static int g2d_vc4_launch_mode(uint32_t code_p, uint32_t unif_p, uint32_t nq,
                               uint32_t *srqcs_out, int flush_writes,
                               int invalidate);

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
#define KERN_GATHER_VC4 8
#define KERN_ALPHA_GATHER_VC4 9
#define KERN_CACHE_SCRUB_VC4 10
#define KERN_FILL_LOOP_VC4 11
/* Self-looping TMU copy kernel: one launch walks whole rows, each QPU
 * thread gathering and storing up to G2D_BAND_MAX_VDW 16-pixel spans
 * (the per-thread iteration cap is enforced by bsp_g2d.c). */
#define KERN_COPY_LOOP_VC4 12
/* Self-looping TMU alpha kernel: copy-loop shape plus a dst gather and
 * the source-over ALU blend - one launch per 12 destination rows under
 * the same eligibility and iteration cap as the copy loop. */
#define KERN_ALPHA_LOOP_VC4 13
#define KERN_TOTAL 14
/* Alpha has one immutable code address per exact output span.  VC4 does
 * not reliably observe either dynamic late-uniform VDW setups or code
 * replacement at an address already present in its instruction cache. */
#define VC4_ALPHA_SPANS 16u
#define VC4_RUN_SLOTS ((KERN_TOTAL - KERN_FILL_VC4) + VC4_ALPHA_SPANS - 1u)
static uint64_t *_kcode[KERN_TOTAL];        /* per-kernel code VA (dma) */
static uint32_t _kcode_p[KERN_TOTAL];       /* per-kernel code physical */
static const uint64_t *_ksrc[KERN_TOTAL];   /* kernel source arrays */
static unsigned _ksrc_n[KERN_TOTAL];        /* kernel instruction counts */
static uint32_t *_unif;            /* uniform staging (CSD_UNIF_WORDS) */
static uint32_t *_scratch;         /* TMU write scratch (16 KiB) */
static uint32_t _unif_p, _scratch_p;
static uint64_t *_nop_code;        /* dma staging of g2d_nop_vc4 */
static uint32_t _nop_code_p;

/* Pi3 bring-up fix: fixed-address VC4 kernel staging.  On BCM2837 the
 * QPU cannot reliably fetch the init-time _kcode staging region: the
 * QPU's own reads there return stale boot-pattern data (0x55555555)
 * while the ARM sees the freshly copied kernel - the SRQ dispatch then
 * wedges permanently (verified by TMU-read + address bisect).  The
 * staging allocated here fetches correctly on silicon.  Each production
 * kernel has its own 2 KiB slot: replacing gather code with alpha code at
 * the same address is not reliably observed by the VC4 instruction cache. */
static uint64_t *_run_code;
static uint32_t _run_code_p;
static uint8_t _vc4_slot_ready[VC4_RUN_SLOTS];
/* Set by prepare_reads() after the caller has rewritten source memory. */
static int _vc4_need_invalidate = 1;
/* The BCM2837 V3D caches sit above a shared system L3 that ARM-side
 * V3D register clears cannot invalidate.  A read-only QPU stream over
 * more than the cache capacity evicts historical GPU-owned lines before
 * kernels read caller memory that the ARM may have rewritten. */
#define VC4_SCRUB_BYTES_PER_QPU (32u * 1024u)
#define VC4_SCRUB_LINES_PER_QPU (VC4_SCRUB_BYTES_PER_QPU / 64u)
static uint32_t *_vc4_scrub_unif;
static uint32_t _vc4_scrub_unif_p;
static uint32_t *_vc4_scrub_mem;
static uint32_t _vc4_scrub_mem_p;
static uint32_t _vc4_scrub_code_p;

/* EwokOS dma_alloc() rounds every request to a page.  A 12-QPU affine
 * batch needs only 768 bytes of address vectors and 1536 bytes of uniforms,
 * but allocating both per batch exhausts the Pi3's 16 MiB sys_dma pool in
 * one 800x600 operation.  Keep the transient data in one reusable arena.
 * Reuse is synchronized by the system-L3 scrub in staging_alloc(). */
#define VC4_STAGING_BYTES (1024u * 1024u)
#define VC4_STAGING_ALIGN 64u
static uint8_t *_vc4_staging;
static uint32_t _vc4_staging_p;
static size_t _vc4_staging_next;
static uint32_t _vc4_staging_wraps;
static uint32_t _vc4_dispatch_seq;

static int _inited = 0;
static int _ok = 0;
static int _ver = 0;               /* architecture version x10 */
static int _num_qpus = 0;
static int _has_hub = 0;
static uint32_t _v3d_clock_hz = 0;

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

/* Firmware property-mailbox transaction with the alias retry cpud uses
 * (machines/raspix/system/drivers/cpud/cpud.c).  The message buffer sits
 * in the NOCACHE sys_dma window; the firmware reads/writes it through
 * the VC bus address carried in the mailbox word.  Firmware versions
 * differ in which SDRAM alias they accept for the message buffer: the
 * bare-metal loader proved the 0xC0000000 coherent alias on BCM2837,
 * so it is tried first; 0x40000000 is the second attempt.  Returns 0
 * when the firmware answered; the caller then validates its own request
 * header (response bits) in the buffer. */
static int g2d_fw_mbox_call(uint32_t buf_phys)
{
    static const uint32_t alias[2] = {
        MAILBOX_VC_ALIAS_COHERENT,
        MAILBOX_VC_ALIAS_NONCACHED,
    };
    uint32_t a;

    for (a = 0; a < 2; a++) {
        mail_message_t msg;

        memset(&msg, 0, sizeof(msg));
        msg.data = ((uint32_t)(buf_phys + alias[a]) >> 4);
        msg.channel = PROPERTY_CHANNEL;
        if (bcm283x_mailbox_call_timeout(&msg, 0) == 0)
            return 0;
    }
    return -1;
}

static int g2d_clock_get(uint32_t property_tag, uint32_t *rate_hz)
{
    g2d_clock_get_req_t *req;
    ewokos_addr_t vaddr;
    ewokos_addr_t phys;
    int result = -1;
    uint32_t attempt;

    if (rate_hz == NULL)
        return -1;
    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0)
        return -1;
    req = (g2d_clock_get_req_t *)(uintptr_t)vaddr;

    phys = dma_phy_addr(0, vaddr);
    /* ewokos_addr_t is 32 bits on the arm build: always sub-4 GB */
    if (phys != 0 && (sizeof(phys) <= 4 || (phys >> 32) == 0)) {
        for (attempt = 0; attempt < 2 && result != 0; attempt++) {
            /* re-arm the request per attempt: a transaction that got a
             * reply but failed validation left response fields in it */
            memset(req, 0, sizeof(*req));
            req->buf_size = sizeof(*req);
            req->tag.tag = property_tag;
            req->tag.value_buf_size = 8;
            req->tag.value_len = 4;
            req->tag.clock_id = FW_CLOCK_V3D;
            if (g2d_fw_mbox_call((uint32_t)phys) == 0 &&
                (req->code & FW_RESPONSE) != 0 &&
                (req->tag.value_len & FW_RESPONSE) != 0 &&
                (req->tag.value_len & ~FW_RESPONSE) >= 4 &&
                req->tag.clock_id == FW_CLOCK_V3D && req->tag.rate_hz != 0) {
                *rate_hz = req->tag.rate_hz;
                result = 0;
            }
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
    int result = -1;
    uint32_t attempt;

    if (rate_hz == 0)
        return -1;
    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0)
        return -1;
    req = (g2d_clock_set_req_t *)(uintptr_t)vaddr;

    phys = dma_phy_addr(0, vaddr);
    if (phys != 0 && (sizeof(phys) <= 4 || (phys >> 32) == 0)) {
        for (attempt = 0; attempt < 2 && result != 0; attempt++) {
            memset(req, 0, sizeof(*req));
            req->buf_size = sizeof(*req);
            req->tag.tag = FW_SET_CLOCK_RATE;
            req->tag.value_buf_size = 12;
            req->tag.value_len = 12;
            req->tag.clock_id = FW_CLOCK_V3D;
            req->tag.rate_hz = rate_hz;
            req->tag.skip_turbo = 0;
            if (g2d_fw_mbox_call((uint32_t)phys) == 0 &&
                (req->code & FW_RESPONSE) != 0 &&
                (req->tag.value_len & FW_RESPONSE) != 0 &&
                (req->tag.value_len & ~FW_RESPONSE) >= 8 &&
                req->tag.clock_id == FW_CLOCK_V3D)
                result = 0;
        }
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
            _v3d_clock_hz = actual_hz;
            slog("g2d: V3D clock forced to %u Hz\r\n", actual_hz);
            return;
        }
        slog("g2d: V3D clock stuck at 0 Hz\r\n");
        return;
    }
    _v3d_clock_hz = actual_hz;
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
    int result = -1;
    uint32_t attempt;

    if (bcm283x_mailbox_init() == 0)
        return -1;
    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0)
        return -1;
    req = (g2d_qpu_enable_req_t *)(uintptr_t)vaddr;

    phys = dma_phy_addr(0, vaddr);
    if (phys != 0 && (sizeof(phys) <= 4 || (phys >> 32) == 0)) {
        for (attempt = 0; attempt < 2 && result != 0; attempt++) {
            memset(req, 0, sizeof(*req));
            req->buf_size = sizeof(*req);
            req->tag.tag = FW_SET_ENABLE_QPU;
            req->tag.value_buf_size = 4;
            req->tag.value_len = 4;
            req->tag.enable = 1;
            if (g2d_fw_mbox_call((uint32_t)phys) == 0 &&
                (req->code & FW_RESPONSE) != 0 &&
                (req->tag.value_len & FW_RESPONSE) != 0)
                result = 0;
        }
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
    int result = -1;
    uint32_t attempt;

    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0)
        return -1;
    req = (g2d_domain_req_t *)(uintptr_t)vaddr;

    phys = dma_phy_addr(0, vaddr);
    if (phys != 0 && (sizeof(phys) <= 4 || (phys >> 32) == 0)) {
        for (attempt = 0; attempt < 2 && result != 0; attempt++) {
            memset(req, 0, sizeof(*req));
            req->buf_size = sizeof(*req);
            req->tag.tag = tag;
            req->tag.value_buf_size = 8;
            req->tag.value_len = 8;
            req->tag.domain = domain_id;
            req->tag.on = 1;
            if (g2d_fw_mbox_call((uint32_t)phys) == 0 &&
                (req->code & FW_RESPONSE) != 0 &&
                (req->tag.value_len & FW_RESPONSE) != 0)
                result = 0;
        }
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

    /* VC4 has no L2TCACTL at 0x30 (that offset is INTCTL).  Its QPU
     * memory traffic is retired by clearing the unified V3D L2 cache. */
    if (_ver == 21) {
        v3d_ctl()[CTL_L2CACTL / 4] = (1u << 2) | (1u << 0);
        for (i = 0; i < 2000000 &&
             (v3d_ctl()[CTL_L2CACTL / 4] & (1u << 2)); i++)
            ;
        g2d_dsb();
        return;
    }

    v3d_ctl()[CTL_L2TCACTL / 4] = (1u << 8);               /* TMUWCF */
    for (i = 0; i < 2000000 && (v3d_ctl()[CTL_L2TCACTL / 4] & (1u << 8)); i++) {}
    v3d_ctl()[CTL_L2TCACTL / 4] = (1u << 0) | (2u << 1);   /* L2TFLS | CLEAN */
    for (i = 0; i < 2000000 && (v3d_ctl()[CTL_L2TCACTL / 4] & (1u << 0)); i++) {}
    g2d_dsb();
}

/* Per-job GPU cache maintenance (the V3D caches do not snoop CPU
 * writes): invalidate the texture L2 + all slice caches so a reused
 * staging buffer or freshly written uniform block is never served
 * stale - the same discipline as the Linux vc4 driver's
 * vc4_flush_caches() before every job. */
static void g2d_invalidate_caches(void)
{
    uint32_t i;

    if (_ver != 21) {
        v3d_ctl()[CTL_L2TFLSTA / 4] = 0;
        v3d_ctl()[CTL_L2TFLEND / 4] = ~0u;
        v3d_ctl()[CTL_L2TCACTL / 4] =
            (1u << 0) | (1u << 1);               /* L2TFLS | CLEAR */
        /* GFXH-1897: a pending L2T flush must complete before any further
         * L2TCACTL write or QPU traffic */
        for (i = 0; i < 2000000 &&
             (v3d_ctl()[CTL_L2TCACTL / 4] & (1u << 0)); i++)
            ;
    }
    v3d_ctl()[CTL_SLCACTL / 4] = 0x0F0F0F0Fu;  /* clear T1/T0/U/I slice caches */
    v3d_ctl()[CTL_L2CACTL / 4] = (1u << 2) | (1u << 0); /* L2CCLR | L2CENA */
    /* L2CCLR is asynchronous.  Returning before it self-clears lets a
     * following Normal-NC CPU write race an old GPU line being retired. */
    for (i = 0; i < 2000000 &&
         (v3d_ctl()[CTL_L2CACTL / 4] & (1u << 2)); i++)
        ;
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
    /* Pi3 bring-up diagnostic: VPM size (IDENT1 bits 31:28) and QPU
     * count (QUPS bits 11:8, NSLC bits 7:4). */
    {
        uint32_t d1 = v3d_ctl()[CTL_IDENT1 / 4];
        slog("g2d: VC4 IDENT1=0x%x VPMsize=%u QUPS=%u NSLC=%u\r\n",
             d1, (uint32_t)((d1 >> 28) & 0xFu),
             (uint32_t)((d1 >> 8) & 0xFu), (uint32_t)((d1 >> 4) & 0xFu));
    }
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
 * V3DRSTN).  Disabled by default on BCM2835/2711 - see
 * G2D_SKIP_PM_RESET. */
#if !G2D_SKIP_PM_RESET
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
#endif

/* Pi3 bring-up fix: the EwokOS kernel generator emits every
 * VPM/VDW setup and address write WITHOUT the write-swap (ws)
 * modifier, so they land in the READ-side special registers
 * (vpmvcd_rd_setup 49-A / vpm_ld_addr 50-A) instead of the
 * write-side ones (vw_setup 49-B / vw_addr 50-B) the VDW engine
 * actually reads.  The launch then completes but the VDW DMA
 * never fires: no data reaches memory.  The known-good DEADBEEF
 * and GPU_FFT shaders carry the ws bit (bit 44 = 0x1000 in the
 * high word); patch every affected word while staging (the
 * generator fix belongs in EwokOS tools/qpuasm.py). */
static void g2d_vc4_patch_vdw(uint64_t *code, uint32_t n)
{
    static const struct { uint64_t from, to; } PATCH[] = {
        { 0x10020c67155a7d80ULL, 0x10021c67155a7d80ULL }, /* vpmvcd setup (fill) */
        { 0x10020c67159e7480ULL, 0x10021c67159e7480ULL }, /* vpmvcd setup (micros) */
        { 0x10020c67159e7000ULL, 0x10021c67159e7000ULL }, /* vdw_setup_0 */
        { 0xe0020c67c0000000ULL, 0xe0021c67c0000000ULL }, /* vdw_setup_1 stride */
        { 0x10020ca7159e7240ULL, 0x10021ca7159e7240ULL }, /* vw_addr (fill) */
        { 0x10020ca715127d80ULL, 0x10021ca715127d80ULL }, /* vw_addr (micros) */
    };
    uint32_t i, j;
    for (i = 0; i < n; i++)
        for (j = 0; j < (uint32_t)(sizeof(PATCH) / sizeof(PATCH[0])); j++)
            if (code[i] == PATCH[j].from)
                code[i] = PATCH[j].to;

    /* Branch instructions do not have ALU write-address fields. */
    for (i = 0; i < n; i++)
        if ((code[i] >> 60) == 0xf)
            code[i] &= ~0x00000fff00000000ULL;

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
    _ksrc[KERN_GATHER_VC4] = g2d_qpu_argb_gather_vc4; _ksrc_n[KERN_GATHER_VC4] = g2d_qpu_argb_gather_vc4_n;
    _ksrc[KERN_ALPHA_GATHER_VC4] = g2d_qpu_argb_alpha_gather_vc4; _ksrc_n[KERN_ALPHA_GATHER_VC4] = g2d_qpu_argb_alpha_gather_vc4_n;
    _ksrc[KERN_CACHE_SCRUB_VC4] = g2d_qpu_cache_scrub_vc4; _ksrc_n[KERN_CACHE_SCRUB_VC4] = g2d_qpu_cache_scrub_vc4_n;
    _ksrc[KERN_FILL_LOOP_VC4] = g2d_qpu_argb_fill_loop_vc4; _ksrc_n[KERN_FILL_LOOP_VC4] = g2d_qpu_argb_fill_loop_vc4_n;
    _ksrc[KERN_COPY_LOOP_VC4] = g2d_qpu_argb_copy_loop_vc4; _ksrc_n[KERN_COPY_LOOP_VC4] = g2d_qpu_argb_copy_loop_vc4_n;
    _ksrc[KERN_ALPHA_LOOP_VC4] = g2d_qpu_argb_alpha_loop_vc4; _ksrc_n[KERN_ALPHA_LOOP_VC4] = g2d_qpu_argb_alpha_loop_vc4_n;
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
    /* Pi3 bring-up fix: fixed-slot VC4 kernel staging in the proven
     * fetch region (see the _run_code declaration).  An 8 KiB pad
     * before it keeps the staging OUT of the nop kernel's own fetch
     * line ([0x2134800,0x2134900)) - staging there fetched stale data
     * and wedged the SRQ; the address reached after the pad
     * (0x21368d0) fetched the full fill kernel cleanly on silicon. */
    (void)dma_alloc(0, CSD_CODE_WORDS * 8u * 4u);   /* pad */
    _run_code = (uint64_t *)(uintptr_t)dma_alloc(
                    0, VC4_RUN_SLOTS * CSD_CODE_WORDS * 8u);
    if (_run_code != 0)
        _run_code_p = (uint32_t)dma_phy_addr(0,
                        (ewokos_addr_t)(uintptr_t)_run_code);
    if (_ver == 21 && _run_code != 0 && _num_qpus > 0 && _num_qpus <= 16) {
        uint32_t q, w;
        uint32_t run_slot = (uint32_t)(KERN_CACHE_SCRUB_VC4 -
                                       KERN_FILL_VC4) +
                            VC4_ALPHA_SPANS - 1u;
        uint64_t *scrub_code = _run_code + run_slot * CSD_CODE_WORDS;

        _vc4_scrub_unif = (uint32_t *)(uintptr_t)dma_alloc(
                              0, (uint32_t)_num_qpus * VC4_UNIF_QWORDS * 4u);
        _vc4_scrub_mem = (uint32_t *)(uintptr_t)dma_alloc(
                             0, (uint32_t)_num_qpus *
                                VC4_SCRUB_BYTES_PER_QPU);
        if (!_vc4_scrub_unif || !_vc4_scrub_mem)
            return -1;
        _vc4_scrub_unif_p = (uint32_t)dma_phy_addr(
                                0, (ewokos_addr_t)(uintptr_t)_vc4_scrub_unif);
        _vc4_scrub_mem_p = (uint32_t)dma_phy_addr(
                                0, (ewokos_addr_t)(uintptr_t)_vc4_scrub_mem);
        if (!_vc4_scrub_unif_p || !_vc4_scrub_mem_p)
            return -1;

        _vc4_staging = (uint8_t *)(uintptr_t)dma_alloc(0,
                                                           VC4_STAGING_BYTES);
        if (!_vc4_staging)
            return -1;
        _vc4_staging_p = (uint32_t)dma_phy_addr(
                              0, (ewokos_addr_t)(uintptr_t)_vc4_staging);
        if (!_vc4_staging_p)
            return -1;
        _vc4_staging_next = 0;
        _vc4_staging_wraps = 0;
        _vc4_dispatch_seq = 0;
        slog("g2d: VC4 staging arena pa=0x%x bytes=%u\r\n",
             _vc4_staging_p, VC4_STAGING_BYTES);
        for (q = 0; q < (uint32_t)_num_qpus; q++) {
            uint32_t *u = _vc4_scrub_unif + q * VC4_UNIF_QWORDS;

            for (w = 0; w < VC4_UNIF_QWORDS; w++)
                u[w] = 0;
            /* Alias 0 is the VPU's normal-allocating system-L3 view. */
            u[0] = _vc4_scrub_mem_p +
                   q * VC4_SCRUB_BYTES_PER_QPU;
            u[1] = VC4_SCRUB_LINES_PER_QPU;
        }
        for (w = 0; w < _ksrc_n[KERN_CACHE_SCRUB_VC4]; w++)
            scrub_code[w] = _ksrc[KERN_CACHE_SCRUB_VC4][w];
        g2d_vc4_patch_vdw(scrub_code,
                          _ksrc_n[KERN_CACHE_SCRUB_VC4]);
        scrub_code[_ksrc_n[KERN_CACHE_SCRUB_VC4] + 0u] =
            0x100009e7009e7000ULL;
        scrub_code[_ksrc_n[KERN_CACHE_SCRUB_VC4] + 1u] =
            0x100009e7009e7000ULL;
        _vc4_scrub_code_p = _run_code_p +
                            run_slot * CSD_CODE_WORDS * 8u;
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
     * first SRQ launch (GPU_FFT does the same at alloc time).  Reserve
     * 4 KiB of VPM before any user shader starts.  V3D_VPMBASE writes
     * made after the first SRQ launch are ignored by VC4, leaving a zero
     * reservation and raising ERRSTAT.VPMEWR on every VPM write. */
    if (_ver == 21) {
        v3d_ctl()[V3D_VPMBASE / 4] = 16u;
        g2d_dsb();
        slog("g2d: VC4 user VPM reserved=%u bytes (VPMBASE=%u)\r\n",
             (uint32_t)(v3d_ctl()[V3D_VPMBASE / 4] & 0x1fu) * 256u,
             (uint32_t)(v3d_ctl()[V3D_VPMBASE / 4] & 0x1fu));
        slog("g2d: VC4 alpha pipeline=three-stage kernel_words=%u\r\n",
             g2d_qpu_argb_alpha_vc4_n);
        slog("g2d: VC4 dispatch=direct-srq\r\n");
        g2d_qpu_enable();
    }

#if G2D_VC4_NOP_TEST
    if (_ver == 21 && _run_code != 0) {
        /* BRING-UP probe (real kernel as dispatch #1): the 5-word nop
         * completes from any staging (init nop staging and run_code
         * slot 0 both verified), but every REAL kernel dispatch wedges
         * (SRQCS 0xc00/0xc0c/0xc90, done=0) whether it is this boot's
         * first dispatch or a later one - so the failure tracks the
         * kernel content (uniform fetch / VDW), not the region or the
         * dispatch count.  Launch the exact-span fill kernel, 1 QPU /
         * 1 group, into the init scratch and verify the pixels: a
         * timeout means the kernel stream itself wedges the SRQ; a
         * missing write means the uniform layout / VDW setup is wrong;
         * ok means real kernels CAN dispatch as dispatch #1. */
        uint32_t u[VC4_UNIF_QWORDS] = { 0 };
        uint32_t srqcs = 0;
        uint32_t color = 0xff204060u;
        u[0] = _scratch_p | 0x80000000u;   /* dst bus addr, DIRECT alias */
        u[1] = color;                      /* ARGB fill value */
        u[2] = 0x80804000u | (16u << 16);  /* one 16-word VDW group */
        if (v3d_g2d_run_vc4(g2d_qpu_argb_fill_vc4,
                            (int)(sizeof(g2d_qpu_argb_fill_vc4) / 8),
                            u, 1, NULL, 0, _scratch, 64) != 0) {
            slog("g2d: VC4 fill16 probe FAILED SRQCS=0x%x\r\n", srqcs);
        } else if (_scratch[0] == color) {
            slog("g2d: VC4 fill16 probe ok pixel=0x%x\r\n",
                 (uint32_t)_scratch[0]);
        } else {
            slog("g2d: VC4 fill16 probe no-write pixel=0x%x "
                 "(expect 0x%x)\r\n", (uint32_t)_scratch[0], color);
        }
    }

#if G2D_VC4_MICRO_TEST
    if (_ver == 21) {
        /* Pi3 bring-up bisection: which instruction stream wedges the
         * SRQ dispatch?  Each micro-kernel runs from freshly-written
         * staging with per-QPU uniforms q, q+1, ...; a TIMEOUT names
         * the first failing stream. */
        static const struct {
            const uint64_t *code; int n;
            const char *name;
        } MICRO[8] = {
            { g2d_t_exit_vc4,  (int)(sizeof(g2d_t_exit_vc4) / 8),  "t_exit" },
            { g2d_t_unif1_vc4, (int)(sizeof(g2d_t_unif1_vc4) / 8), "t_unif1" },
            { g2d_t_unif9_vc4, (int)(sizeof(g2d_t_unif9_vc4) / 8), "t_unif9" },
            { g2d_t_branch_vc4,(int)(sizeof(g2d_t_branch_vc4) / 8),"t_branch" },
            { g2d_t_bloop_vc4, (int)(sizeof(g2d_t_bloop_vc4) / 8), "t_bloop" },
            { g2d_t_store_vc4, (int)(sizeof(g2d_t_store_vc4) / 8), "t_store" },
            { g2d_t_store2_vc4,(int)(sizeof(g2d_t_store2_vc4) / 8),"t_store2" },
            { g2d_t_store3_vc4,(int)(sizeof(g2d_t_store3_vc4) / 8),"t_store3" },
        };
        uint32_t m, q, srqcs = 0;
        /* Pi3 bring-up fix: stage from _run_code (proven fetch region
         * at 0x21368d0) instead of a fresh allocation adjacent to the
         * nop staging - launches there fetched stale data. */
        if (_run_code != 0) {
            for (m = 0; m < 8; m++) {
                uint32_t k;
                for (k = 0; k < (uint32_t)MICRO[m].n; k++)
                    _run_code[k] = MICRO[m].code[k];
                g2d_vc4_patch_vdw(_run_code, (uint32_t)MICRO[m].n);
                for (q = 0; q < 16; q++) {
                    uint32_t *s = _unif + q * VC4_UNIF_QWORDS;
                    s[0] = 0x11223344u;             /* color / dst */
                    s[1] = q;                       /* value / qid */
                    s[2] = 0;                       /* L-1 */
                    s[3] = _scratch_p | MAILBOX_VC_ALIAS_NONCACHED;
                    s[4] = 0; s[5] = 16; s[6] = 0;
                    s[7] = 4;                       /* iters */
                    s[8] = 0;                       /* gx0 */
                }
                g2d_invalidate_caches();
                if (g2d_vc4_launch(_run_code_p, _unif_p,
                                   (uint32_t)_num_qpus, &srqcs) != 0) {
                    v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                    slog("g2d: VC4 micro %s TIMEOUT SRQCS=0x%x\r\n",
                         MICRO[m].name, srqcs);
                } else {
                    slog("g2d: VC4 micro %s ok scratch0=0x%x store=0x%x\r\n",
                         MICRO[m].name, (uint32_t)_scratch[0],
                         (uint32_t)*(volatile uint32_t *)(uintptr_t)0x11122334);
                }
            }
            /* TMU view check: what does the QPU see at the kcode fill
             * staging?  If the alias read differs from the ARM view,
             * the instruction fetch there is stale (L2/alias issue). */            {
                static const uint32_t TMUADDR[2] = { 0, 1 };  /* alias? */
                uint32_t t, k;
                for (k = 0; k < (uint32_t)g2d_qpu_argb_fill_vc4_n; k++)
                    _run_code[k] = g2d_t_tmuread_vc4[k];
                g2d_vc4_patch_vdw(_run_code, (uint32_t)g2d_qpu_argb_fill_vc4_n);
                for (t = 0; t < 2; t++) {
                    /* Pi3 bring-up fix: the TMU read source used to be
                     * the kcode staging, which is NOT reliably fetchable
                     * on BCM2837 - the QPUs intermittently wedged the
                     * whole SRQ there (SRQCS stuck at 0xc90).  Read the
                     * kernel's own first word from the proven _run_code
                     * region instead; the ARM compares against it. */
                    uint32_t addr = _run_code_p
                                    | (TMUADDR[t] ? MAILBOX_VC_ALIAS_NONCACHED : 0u);
                    for (q = 0; q < 16; q++) {
                        uint32_t *s = _unif + q * VC4_UNIF_QWORDS;
                        s[0] = addr;
                        s[1] = (_scratch_p + q * 64u)
                               | MAILBOX_VC_ALIAS_NONCACHED;
                        s[2] = 0; s[3] = 0; s[4] = 0; s[5] = 0;
                        s[6] = 0; s[7] = 0; s[8] = 0;
                    }
                    g2d_invalidate_caches();
                    if (g2d_vc4_launch(_run_code_p, _unif_p,
                                       (uint32_t)_num_qpus, &srqcs) != 0) {
                        v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                        slog("g2d: VC4 tmu@%s TIMEOUT SRQCS=0x%x\r\n",
                             TMUADDR[t] ? "alias" : "plain", srqcs);
                    } else {
                        slog("g2d: VC4 tmu@%s got 0x%x (arm 0x%x)\r\n",
                             TMUADDR[t] ? "alias" : "plain",
                             (uint32_t)_scratch[0],
                             (uint32_t)(_run_code[0] & 0xFFFFFFFFu));
                    }
                }
            }

            /* store-count sweep (fresh state, full fill kernel from
             * mcode): groups per row via l1, rows; 1 QPU and 12 QPUs.
             * G2D_VC4_SWEEP_ONLY_FIRST runs just entry 0 so a single
             * geometry can be tested on a pristine V3D. */
            {
                static const struct { uint32_t nq, l1, rows; } SWEEP[8] = {
                    { 12, 3, 4 }, { 12, 0, 1 }, { 12, 1, 1 }, { 12, 2, 1 },
                    { 12, 3, 1 }, { 1, 3, 4 }, { 4, 3, 4 }, { 12, 1, 2 },
                };
                uint32_t w, wend;
                wend = G2D_VC4_SWEEP_ONLY_FIRST ? 1 : 8;
                for (w = 0; w < wend; w++) {
                    uint32_t k;
                    for (k = 0; k < (uint32_t)g2d_qpu_argb_fill_vc4_n; k++)
                        _run_code[k] = g2d_qpu_argb_fill_vc4[k];
                    for (q = 0; q < SWEEP[w].nq; q++) {
                        uint32_t *s = _unif + q * VC4_UNIF_QWORDS;
                        s[0] = 0x11223344u;
                        s[1] = q;
                        s[2] = SWEEP[w].l1;
                        s[3] = (_scratch_p + q * SWEEP[w].rows * 64u * 4u)
                               | MAILBOX_VC_ALIAS_NONCACHED;
                        s[4] = 0; s[5] = 64; s[6] = 0;
                        s[7] = SWEEP[w].rows;
                        s[8] = 0;
                    }
                    g2d_invalidate_caches();
                    if (g2d_vc4_launch(_run_code_p, _unif_p,
                                       SWEEP[w].nq, &srqcs) != 0) {
                        v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                        slog("g2d: VC4 sweep nq=%u l1=%u rows=%u "
                             "TIMEOUT SRQCS=0x%x\r\n", SWEEP[w].nq,
                             SWEEP[w].l1, SWEEP[w].rows, srqcs);
                    } else {
                        slog("g2d: VC4 sweep nq=%u l1=%u rows=%u ok "
                             "(scratch 0x%x 0x%x)\r\n", SWEEP[w].nq,
                             SWEEP[w].l1, SWEEP[w].rows,
                             (uint32_t)_scratch[0], (uint32_t)_scratch[1]);
                    }
                }
            }

            /* full kernel, rows=0 (no stores): launch the SAME fill
             * kernel from EVERY staging address, once with the VC
             * 0x40000000 non-allocating alias (as the dispatch path
             * uses) and once PLAIN - GPU_FFT fetches its code from the
             * plain L2-cached view, so a plain fetch succeeding where
             * the alias fetch wedges pinpoints the alias as the bug
             * (address-bisect diagnostic). */
#if G2D_VC4_BISECT
            {
                static const char *BISECT[18] = {
                    "k0alias 0x6212c090", "k0plain 0x0212c090",
                    "k1alias 0x6212c890", "k1plain 0x0212c890",
                    "k2alias 0x6212d090", "k2plain 0x0212d090",
                    "k3alias 0x6212d890", "k3plain 0x0212d890",
                    "k4alias 0x6212e090", "k4plain 0x0212e090",
                    "k5alias 0x6212e890", "k5plain 0x0212e890",
                    "k6alias 0x6212f090", "k6plain 0x0212f090",
                    "k7alias 0x6212f890", "k7plain 0x0212f890",
                    "nopalias 0x62134890", "mcodealias 0x621348d0",
                };
                uint32_t t;
                for (t = 0; t < 18; t++) {
                    uint32_t k;
                    uint32_t code_p;
                    if (t < 16) {
                        uint32_t slot = t >> 1;
                        uint32_t plain = t & 1;
                        code_p = (plain ? 0u : MAILBOX_VC_ALIAS_NONCACHED)
                                 | (_kcode_p[slot] & ~MAILBOX_VC_ALIAS_NONCACHED);
                    } else if (t == 16) {
                        code_p = _nop_code_p
                                 | MAILBOX_VC_ALIAS_NONCACHED;
                    } else {
                        code_p = _run_code_p | MAILBOX_VC_ALIAS_NONCACHED;
                    }
                    /* staging slot at mcode already holds the fill; copy
                     * into the other slots (same code every time) */
                    for (k = 0; k < (uint32_t)g2d_qpu_argb_fill_vc4_n; k++)
                        ((uint64_t *)(uintptr_t)(code_p
                                    & ~MAILBOX_VC_ALIAS_NONCACHED))[k] =
                            g2d_qpu_argb_fill_vc4[k];
                    for (q = 0; q < 16; q++) {
                        uint32_t *s = _unif + q * VC4_UNIF_QWORDS;
                        s[0] = 0x11223344u;
                        s[1] = q;
                        s[2] = 0;
                        s[3] = _scratch_p | MAILBOX_VC_ALIAS_NONCACHED;
                        s[4] = 0; s[5] = 16; s[6] = 0;
                        s[7] = 0;   /* rows: no stores */
                        s[8] = 0;
                    }
                    g2d_invalidate_caches();
                    if (g2d_vc4_launch(code_p, _unif_p,
                                       (uint32_t)_num_qpus, &srqcs) != 0) {
                        v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                        slog("g2d: VC4 bisect %-18s TIMEOUT SRQCS=0x%x\r\n",
                             BISECT[t], srqcs);
                    } else {
                        slog("g2d: VC4 bisect %-18s ok\r\n", BISECT[t]);
                    }
                }
            }
#endif  /* G2D_VC4_BISECT */
    }
    }
#endif

    if (G2D_VC4_PROBES && _ver == 21) {
        /* Staged bring-up probes on the REAL fill kernel, one failure
         * domain at a time:
         *   P1 rows_q=0: uniform stream + branch + exit, no VPM/DMA
         *   P2 16x1 into scratch via the PLAIN dst address
         *   P3 16x1 into scratch via the 0x40000000 alias
         *   P4 like P3, then L2CCLR before the readback
         *   P5 like P3, then 500 ms drain wait before the readback
         * The scratch[0] values after P2-P5 tell us whether the QPU's
         * VPM/VDW stores reach DRAM at all (P2), only via the alias
         * (P3), only after an L2 clean (P4), or after a drain (P5). */
        static const struct { uint32_t l1, x1, rows; uint32_t mode; }
        PRB[7] = {
            { 0, 16, 0, 0 },        /* P1: no rows, exit at once */
            { 0, 16, 1, 0 },        /* P2: plain dst */
            { 0, 16, 1, 1 },        /* P3: alias dst */
            { 0, 16, 1, 2 },        /* P4: alias dst + L2CCLR */
            { 0, 16, 1, 3 },        /* P5: alias dst + 500ms wait */
            { 0, 16, 1, 4 },        /* P6: 0x80000000 DIRECT dst (no L2) */
            { 0, 16, 1, 5 },        /* P7: direct dst + L2CCLR */
        };
        uint32_t prb, q, srqcs = 0;

        /* Pi3 bring-up fix: launch from the proven _run_code staging
         * (the init-time kcode region is not reliably fetchable on
         * BCM2837 - see the _run_code declaration). */
        if (_run_code == 0)
            return 0;
        for (prb = 0; prb < 7; prb++) {
            uint32_t i, base;
            for (i = 0; i < (uint32_t)_ksrc_n[KERN_FILL_VC4]; i++)
                _run_code[i] = _ksrc[KERN_FILL_VC4][i];
            g2d_vc4_patch_vdw(_run_code, (uint32_t)_ksrc_n[KERN_FILL_VC4]);
            for (q = 0; q < 16; q++) {
                uint32_t *s = _unif + q * VC4_UNIF_QWORDS;
                s[0] = 0x12345678u;             /* color */
                s[1] = q;                       /* qid */
                s[2] = PRB[prb].l1;             /* L-1 */
                base = _scratch_p + q * PRB[prb].rows * PRB[prb].x1 * 4u;
                if (PRB[prb].mode == 0)
                    s[3] = base;
                else if (PRB[prb].mode == 1 || PRB[prb].mode == 2 ||
                         PRB[prb].mode == 3)
                    s[3] = base | MAILBOX_VC_ALIAS_NONCACHED;
                else
                    s[3] = base | 0x80000000u;  /* DIRECT (no L2) */
                s[4] = 0;                       /* x0 */
                s[5] = PRB[prb].x1;             /* x1 */
                s[6] = 0;                       /* rowjump */
                s[7] = PRB[prb].rows;           /* rows_q */
                s[8] = 0;                       /* gx0 */
            }
            g2d_invalidate_caches();
            if (g2d_vc4_launch(_run_code_p, _unif_p,
                               (uint32_t)_num_qpus, &srqcs) != 0) {
                v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                slog("g2d: VC4 probe P%u TIMEOUT SRQCS=0x%x\r\n",
                     prb + 1, srqcs);
                continue;
            }
            if (PRB[prb].mode == 2 || PRB[prb].mode == 5) {
                /* force the QPU's L2C-dirty writes out to DRAM */
                v3d_ctl()[V3D_L2CACTL / 4] = 1u << 2;
            } else if (PRB[prb].mode == 3) {
                uint32_t t;
                for (t = 0; t < 50000000; t++) { }  /* ~0.5s @ 300MHz */
            }
            g2d_dsb();
            slog("g2d: VC4 probe P%u ok scratch0=0x%x\r\n",
                 prb + 1, (uint32_t)_scratch[0]);
        }

        /* P8: Carl Chatfield's DEADBEEF shader (known-good, proven on
         * BCM2835) launched VERBATIM - a control proving the VPM/VDW
         * store path works on this silicon when the ws bits are right
         * (its vw_setup/vw_addr writes all carry the ws modifier).
         * One QPU, the output address from the first uniform. */
        {
            static const uint64_t DEADBEEF[9] = {
                0xe0021c6700101a00ULL,   /* mov vw_setup, vpm_setup(1,1,h32(0)) */
                0xe0020c27deadbeefULL,   /* mov vpm, #0xdeadbeef */
                0x100009e7159f2fc0ULL,   /* vw_wait */
                0xe0021c6780844000ULL,   /* mov vw_setup, vdw_setup_0(1,4,h32(0,0)) */
                0x1002082715827d80ULL,   /* mov out_addr, unif */
                0x10021ca7159e7000ULL,   /* mov vw_addr, out_addr  (ws!) */
                0x100009e7159f2fc0ULL,   /* vw_wait */
                0x300009e7009e7000ULL,   /* nop; thrend */
                0x100009e7009e7000ULL,   /* nop */
            };
            uint32_t k;
            for (k = 0; k < 9; k++)
                _run_code[k] = DEADBEEF[k];
            _unif[0] = _scratch_p;      /* plain address, as DEADBEEF does */
            g2d_invalidate_caches();
            if (g2d_vc4_launch(_run_code_p, _unif_p, 1, &srqcs) != 0) {
                v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                slog("g2d: VC4 probe P8 deadbeef TIMEOUT SRQCS=0x%x\r\n", srqcs);
            } else {
                slog("g2d: VC4 probe P8 deadbeef -> 0x%x 0x%x 0x%x 0x%x\r\n",
                     (uint32_t)_scratch[0], (uint32_t)_scratch[1],
                     (uint32_t)_scratch[2], (uint32_t)_scratch[3]);
            }
            /* P9: DEADBEEF + L2C clean after the launch - if the writes
             * sit dirty in the VC L2C, the ARM only sees them after a
             * clean (the L2CCLR used by the Linux vc4 driver). */
            _unif[0] = _scratch_p + 0x40;
            g2d_invalidate_caches();
            if (g2d_vc4_launch(_run_code_p, _unif_p, 1, &srqcs) != 0) {
                v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                slog("g2d: VC4 probe P9 deadbeef+L2CCLR TIMEOUT SRQCS=0x%x\r\n",
                     srqcs);
            } else {
                v3d_ctl()[V3D_L2CACTL / 4] = 1u << 2;   /* L2CCLR */
                g2d_dsb();
                slog("g2d: VC4 probe P9 deadbeef+L2CCLR -> 0x%x 0x%x 0x%x 0x%x\r\n",
                     (uint32_t)_scratch[0x10], (uint32_t)_scratch[0x11],
                     (uint32_t)_scratch[0x12], (uint32_t)_scratch[0x13]);
            }
            /* P10: DEADBEEF with the VPM setup in v32 mode (GPU_FFT's
             * VPM mode 0x200 instead of h32's 0xa00). */
            {
                uint32_t k;
                uint64_t v32;
                for (k = 0; k < 9; k++)
                    _run_code[k] = DEADBEEF[k];
                _run_code[0] = 0xe0021c6700101200ULL;  /* vpm_setup v32(0,0) */
                (void)v32;
                _unif[0] = _scratch_p + 0x80;
                g2d_invalidate_caches();
                if (g2d_vc4_launch(_run_code_p, _unif_p, 1, &srqcs) != 0) {
                    v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                    slog("g2d: VC4 probe P10 deadbeef-v32 TIMEOUT SRQCS=0x%x\r\n",
                         srqcs);
                } else {
                    slog("g2d: VC4 probe P10 deadbeef-v32 -> 0x%x 0x%x 0x%x 0x%x\r\n",
                         (uint32_t)_scratch[0x20], (uint32_t)_scratch[0x21],
                         (uint32_t)_scratch[0x22], (uint32_t)_scratch[0x23]);
                }
            }
            /* P11: the fill (16px, 1 QPU) + L2C clean after the launch. */
            {
                uint32_t i;
                for (i = 0; i < (uint32_t)_ksrc_n[KERN_FILL_VC4]; i++)
                    _run_code[i] = _ksrc[KERN_FILL_VC4][i];
                g2d_vc4_patch_vdw(_run_code, (uint32_t)_ksrc_n[KERN_FILL_VC4]);
                for (q = 0; q < 16; q++) {
                    uint32_t *s = _unif + q * VC4_UNIF_QWORDS;
                    s[0] = 0x12345678u; s[1] = q; s[2] = 0;
                    s[3] = _scratch_p + 0xC0; s[4] = 0; s[5] = 16;
                    s[6] = 0; s[7] = 1; s[8] = 0;
                }
                g2d_invalidate_caches();
                if (g2d_vc4_launch(_run_code_p, _unif_p, 1, &srqcs) != 0) {
                    v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                    slog("g2d: VC4 probe P11 fill+L2CCLR TIMEOUT SRQCS=0x%x\r\n",
                         srqcs);
                } else {
                    v3d_ctl()[V3D_L2CACTL / 4] = 1u << 2;
                    g2d_dsb();
                    slog("g2d: VC4 probe P11 fill+L2CCLR -> 0x%x 0x%x 0x%x\r\n",
                         (uint32_t)_scratch[0x30], (uint32_t)_scratch[0x31],
                         (uint32_t)_scratch[0x33]);
                }
            }
            /* P12: the fill with the stride word patched to a nop
             * (DEADBEEF shape: no vdw_setup_1) + L2C clean. */
            {
                uint32_t i, k;
                for (i = 0; i < (uint32_t)_ksrc_n[KERN_FILL_VC4]; i++)
                    _run_code[i] = _ksrc[KERN_FILL_VC4][i];
                g2d_vc4_patch_vdw(_run_code, (uint32_t)_ksrc_n[KERN_FILL_VC4]);
                for (k = 0; k < (uint32_t)_ksrc_n[KERN_FILL_VC4]; k++)
                    if (_run_code[k] == 0xe0021c67c0000000ULL ||
                        _run_code[k] == 0xe0020c67c0000000ULL)
                        _run_code[k] = 0x100009e7009e7000ULL; /* nop */
                for (q = 0; q < 16; q++) {
                    uint32_t *s = _unif + q * VC4_UNIF_QWORDS;
                    s[0] = 0x12345678u; s[1] = q; s[2] = 0;
                    s[3] = _scratch_p + 0x100; s[4] = 0; s[5] = 16;
                    s[6] = 0; s[7] = 1; s[8] = 0;
                }
                g2d_invalidate_caches();
                if (g2d_vc4_launch(_run_code_p, _unif_p, 1, &srqcs) != 0) {
                    v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                    slog("g2d: VC4 probe P12 fill-nostride TIMEOUT SRQCS=0x%x\r\n",
                         srqcs);
                } else {
                    v3d_ctl()[V3D_L2CACTL / 4] = 1u << 2;
                    g2d_dsb();
                    slog("g2d: VC4 probe P12 fill-nostride -> 0x%x 0x%x 0x%x\r\n",
                         (uint32_t)_scratch[0x40], (uint32_t)_scratch[0x41],
                         (uint32_t)_scratch[0x43]);
                }
            }
            /* P13: DEADBEEF with the destination at the L2C-bypassing
             * 0x40000000 alias - the VDW's data then lands in DRAM
             * directly and the ARM reads the VPM content verbatim
             * (no L2C write-back artifacts to decode). */
            {
                uint32_t k;
                for (k = 0; k < 9; k++)
                    _run_code[k] = DEADBEEF[k];
                _unif[0] = (_scratch_p + 0x140) | 0x40000000u;
                g2d_invalidate_caches();
                if (g2d_vc4_launch(_run_code_p, _unif_p, 1, &srqcs) != 0) {
                    v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                    slog("g2d: VC4 probe P13 deadbeef-alias TIMEOUT SRQCS=0x%x\r\n",
                         srqcs);
                } else {
                    slog("g2d: VC4 probe P13 deadbeef-alias -> 0x%x 0x%x 0x%x 0x%x\r\n",
                         (uint32_t)_scratch[0x50], (uint32_t)_scratch[0x51],
                         (uint32_t)_scratch[0x52], (uint32_t)_scratch[0x53]);
                }
            }
            /* P14: DEADBEEF with the GPU_FFT's VPM setup
             * vpm_setup(16,1,v32(0,0)) = 0x1001200 (the proven Pi-3
             * store shape) instead of the h32 0x101a00. */
            {
                uint32_t k;
                for (k = 0; k < 9; k++)
                    _run_code[k] = DEADBEEF[k];
                _run_code[0] = 0xe0021c6700101200ULL;
                _unif[0] = _scratch_p + 0x180;
                g2d_invalidate_caches();
                if (g2d_vc4_launch(_run_code_p, _unif_p, 1, &srqcs) != 0) {
                    v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                    slog("g2d: VC4 probe P14 deadbeef-v32n16 TIMEOUT SRQCS=0x%x\r\n",
                         srqcs);
                } else {
                    slog("g2d: VC4 probe P14 deadbeef-v32n16 -> 0x%x 0x%x 0x%x 0x%x\r\n",
                         (uint32_t)_scratch[0x60], (uint32_t)_scratch[0x61],
                         (uint32_t)_scratch[0x62], (uint32_t)_scratch[0x63]);
                }
            }
            /* P15: DEADBEEF with the deadbeef README's own VPM setup
             * 0x00001b00 (h16-style) which the tutorial text shows
             * writing 0xdeadbeef rows. */
            {
                uint32_t k;
                for (k = 0; k < 9; k++)
                    _run_code[k] = DEADBEEF[k];
                _run_code[0] = 0xe0021c6700001b00ULL;
                _unif[0] = _scratch_p + 0x1C0;
                g2d_invalidate_caches();
                if (g2d_vc4_launch(_run_code_p, _unif_p, 1, &srqcs) != 0) {
                    v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                    slog("g2d: VC4 probe P15 deadbeef-1b00 TIMEOUT SRQCS=0x%x\r\n",
                         srqcs);
                } else {
                    slog("g2d: VC4 probe P15 deadbeef-1b00 -> 0x%x 0x%x 0x%x 0x%x\r\n",
                         (uint32_t)_scratch[0x70], (uint32_t)_scratch[0x71],
                         (uint32_t)_scratch[0x72], (uint32_t)_scratch[0x73]);
                }
            }
            /* P16: VPM READ-BACK (vc4asm-assembled): write 0xdeadbeef
             * into the VPM (h32 row 0), read it straight back through
             * the VPM read port, then ship the READ value out through
             * the VDW.  Output == 0xdeadbeef proves the VPM write
             * itself works (and that the VDW corruption happens at the
             * VPM read / write-back stage); output == the 0x55-family
             * proves the VPM write never lands. */
            {
                static const uint64_t RDBK[15] = {
                    0xe0021c6700101a00ULL,   /* mov vw_setup, vpm_setup(1,1,h32(0)) */
                    0xe0020c27deadbeefULL,   /* mov vpm, #0xdeadbeef */
                    0x100009e7159f2fc0ULL,   /* vw_wait */
                    0xe0020c6700101a00ULL,   /* mov vr_setup, vpm_setup(1,1,h32(0)) */
                    0x1002086715c27d80ULL,   /* mov r1, vpm */
                    0x100009e7159f2fc0ULL,   /* vw_wait */
                    0x10020c27159e7240ULL,   /* mov vpm, r1 */
                    0x100009e7159f2fc0ULL,   /* vw_wait */
                    0xe0021c6780844000ULL,   /* mov vw_setup, vdw_setup_0(1,4,dma_h32(0,0)) */
                    0x1002082715827d80ULL,   /* mov out_addr, unif */
                    0x10021ca7159e7000ULL,   /* mov vw_addr, out_addr (ws) */
                    0x100009e7159f2fc0ULL,   /* vw_wait */
                    0x300009e7009e7000ULL,   /* nop; thrend */
                    0x100009e7009e7000ULL,   /* nop */
                    0x100009e7009e7000ULL,   /* nop */
                };
                uint32_t k;
                for (k = 0; k < 15; k++)
                    _run_code[k] = RDBK[k];
                _unif[0] = _scratch_p + 0x200;
                g2d_invalidate_caches();
                if (g2d_vc4_launch(_run_code_p, _unif_p, 1, &srqcs) != 0) {
                    v3d_ctl()[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
                    slog("g2d: VC4 probe P16 readback TIMEOUT SRQCS=0x%x\r\n",
                         srqcs);
                } else {
                    slog("g2d: VC4 probe P16 readback -> 0x%x 0x%x 0x%x 0x%x\r\n",
                         (uint32_t)_scratch[0x80], (uint32_t)_scratch[0x81],
                         (uint32_t)_scratch[0x82], (uint32_t)_scratch[0x83]);
                }
            }
        }
    }
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

uint32_t v3d_g2d_clock_hz(void)
{
    return _v3d_clock_hz;
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
    if (src && src_len)
        g2d_dsb();
    if (dst && dst_len)
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
    if (dst && dst_len)
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
 * read slot 0's words.  Addresses carry the 0x40000000 VC bus alias -
 * the same alias GPU_FFT uses on Pi2/3 for its SRQUA/SRQPC values.
 * The caller must have run g2d_invalidate_caches first so the freshly
 * written staging is fetched from DRAM.  On timeout SRQCS is captured
 * through `srqcs_out`, then the backend is disabled because VC4 has no safe
 * user-program preemption.  Returns 0 on completion, 1 on timeout. */
static int g2d_vc4_launch_mode(uint32_t code_p, uint32_t unif_p, uint32_t nq,
                               uint32_t *srqcs_out, int flush_writes,
                               int invalidate)
{
    volatile uint32_t *core = v3d_ctl();
    uint32_t i, q;

    core[V3D_DBCFG / 4] = 0;
    core[V3D_DBQITE / 4] = 0;
    core[V3D_DBQITC / 4] = ~0u;
    core[0x030 / 4] = ~0u;                  /* INTCTL: clear pending IRQs */
    core[0xf20 / 4] = ~0u;                  /* ERR_STAT: clear sticky bits */
    /* A completed SRQ launch is fully retired before this function returns.
     * Older bring-up code inserted a fixed 200k-iteration delay here to
     * mask a first-dispatch race; that delay is unnecessary once SRQCS and
     * L2C completion are polled below, and it dominates small VC4 jobs. */
    /* GPU_FFT clears the L2 and slice caches before every launch; the
     * nop probe works without them, but the real kernels were fetched
     * stale and wedged at their first uniform read on Pi 3 - Pi3
     * bring-up fix (verified on BCM2837).  Poll the L2CCLR completion
     * bit (it self-clears when the invalidate finishes) before
     * dispatching: the known-good VDW probe times out if the QPU fetch
     * races the L2 clear. */
    if (invalidate) {
        core[V3D_L2CACTL / 4] = (1u << 2) | (1u << 0); /* L2CCLR|L2CENA */
        for (i = 0; i < 100000 && (core[V3D_L2CACTL / 4] & (1u << 2)); i++)
            ;
        core[V3D_SLCACTL / 4] = 0x0F0F0F0Fu;
    }
    core[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
    /* Wait for all SRQ state, not only the done counter, to clear.  On
     * BCM2837 a back-to-back launch can otherwise inherit request slots
     * from the preceding dispatch and silently drop its last QPUs. */
    for (i = 0; i < 100000 &&
         (core[V3D_SRQCS / 4] & 0x00FFFF80u) != 0; i++)
        ;
    core[V3D_SRQUL / 4] = 1024;  /* unlimited uniforms per QPU */
    for (q = 0; q < nq; q++) {
        core[V3D_SRQUA / 4] = (unif_p + q * (VC4_UNIF_QWORDS * 4u))
                              | MAILBOX_VC_ALIAS_NONCACHED;
        core[V3D_SRQPC / 4] = code_p
                              | (G2D_VC4_PLAIN_SRQPC ? 0u
                                                     : MAILBOX_VC_ALIAS_NONCACHED);
    }

    /* SRQCS bits 23:16 count completed threads */
    for (i = 0; i < 2000000; i++)
        if (((core[V3D_SRQCS / 4] >> 16) & 0xFFu) == nq)
            break;
    if (i == 2000000) {
        /* Pi3 bring-up diagnostic: where is the wedged QPU?  Reading
         * SRQPC returns the current instruction address of the last
         * QPU to touch the SRQ; SRQUL the uniforms it has left. */
        uint32_t pc = core[V3D_SRQPC / 4];
        uint32_t ul = core[V3D_SRQUL / 4];
        if (srqcs_out)
            *srqcs_out = core[V3D_SRQCS / 4];
        slog("g2d: VC4 launch TIMEOUT seq=%u SRQCS=0x%x "
             "SRQPC=0x%x SRQUL=0x%x ERR=0x%x SQ0=0x%x SQ1=0x%x "
             "VPA=0x%x\r\n",
             _vc4_dispatch_seq, srqcs_out ? *srqcs_out : 0u, pc, ul,
             core[0xf20 / 4], core[V3D_SQRSV0 / 4],
             core[V3D_SQRSV1 / 4], core[V3D_VPACNTL / 4]);
        core[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
        _ok = 0;
        return 1;
    }
    /* Clear only the W1C done/request/error counters.  SRQCS bit 0 clears
     * the user-program FIFO itself and corrupts a later launch on BCM2837,
     * even when written after the completion count reaches nq. */
    core[V3D_SRQCS / 4] = V3D_SRQCS_CLEAR;
    for (i = 0; i < 100000 &&
         (core[V3D_SRQCS / 4] & 0x00FFFF80u) != 0; i++)
        ;
    if (flush_writes)
        g2d_flush_l2();
    /* BCM2837 may retain VDW stores even when the destination uses the
     * direct bus alias.  Clean the L2C before returning so CPU and later
     * QPU reads observe completed destination writes. */
    core[V3D_L2CACTL / 4] = 1u << 2;               /* L2CCLR */
    for (i = 0; i < 100000 &&
         (core[V3D_L2CACTL / 4] & (1u << 2)); i++)
        ;
    return 0;
}

static int g2d_vc4_launch(uint32_t code_p, uint32_t unif_p, uint32_t nq,
                          uint32_t *srqcs_out)
{
    return g2d_vc4_launch_mode(code_p, unif_p, nq, srqcs_out, 1, 1);
}

int v3d_g2d_vc4_prepare_reads(void)
{
    uint32_t srqcs = 0;

    if (!_ok || _ver != 21 || !_vc4_scrub_code_p ||
        !_vc4_scrub_unif_p || !_vc4_scrub_mem ||
        _num_qpus <= 0 || _num_qpus > 16)
        return -1;
    _vc4_need_invalidate = 1;
    g2d_dsb();
    return g2d_vc4_launch_mode(_vc4_scrub_code_p, _vc4_scrub_unif_p,
                               (uint32_t)_num_qpus, &srqcs, 0, 1) == 0 ?
           0 : -1;
}

void *v3d_g2d_vc4_staging_alloc(size_t bytes, uint32_t *phys_out)
{
    size_t aligned;
    uint8_t *ret;

    if (!_ok || _ver != 21 || !_vc4_staging || !phys_out || bytes == 0)
        return NULL;
    if (bytes > SIZE_MAX - (VC4_STAGING_ALIGN - 1u))
        return NULL;
    aligned = (bytes + VC4_STAGING_ALIGN - 1u) &
              ~(size_t)(VC4_STAGING_ALIGN - 1u);
    if (aligned > VC4_STAGING_BYTES)
        return NULL;

    if (_vc4_staging_next > VC4_STAGING_BYTES - aligned) {
        /* Every prior user batch has completed before its caller asks for
         * another block.  Evict the system L3 before reusing old bus
         * addresses, which BCM2837 otherwise may serve with stale data. */
        if (v3d_g2d_vc4_prepare_reads() != 0)
            return NULL;
        _vc4_staging_next = 0;
        _vc4_staging_wraps++;
    }
    ret = _vc4_staging + _vc4_staging_next;
    *phys_out = _vc4_staging_p + (uint32_t)_vc4_staging_next;
    _vc4_staging_next += aligned;
    return ret;
}

/* Launch one SRQ thread per QPU of `code` (GPU_FFT protocol): stage
 * per-QPU uniforms + code address, then poll SRQCS until every thread
 * completed.  The uniform stream is per-QPU: QPU q reads its words at
 * _unif + q * VC4_UNIF_QWORDS. */
uint32_t v3d_g2d_vc4_stage_at(const uint64_t *code, int nwords, int off)
{
    uint32_t i;

    if (code == NULL || nwords <= 0 || nwords > CSD_CODE_WORDS ||
        off < 0 || off + nwords > CSD_CODE_WORDS ||
        !_ok || _ver != 21 || _run_code == 0)
        return 0;
    for (i = 0; i < (uint32_t)nwords; i++)
        _run_code[off + i] = code[i];
    /* drop stale L2/slice lines so the freshly staged kernels are
     * fetched from DRAM (same discipline as the micro-test battery) */
    g2d_invalidate_caches();
    return _run_code_p + (uint32_t)off * 8u;
}

int v3d_g2d_vc4_run_staged(uint32_t code_p, const uint32_t *unifs,
                           int num_qpus, uint32_t *srqcs_out)
{
    uint32_t nq = (uint32_t)num_qpus;
    uint32_t i, q;

    if (code_p < _run_code_p ||
        code_p >= _run_code_p + CSD_CODE_WORDS * 8u ||
        unifs == NULL || nq == 0 || nq > 16 || !_ok || _ver != 21)
        return -1;

    for (q = 0; q < nq; q++)
        for (i = 0; i < VC4_UNIF_QWORDS; i++)
            _unif[q * VC4_UNIF_QWORDS + i] =
                unifs[q * VC4_UNIF_QWORDS + i];
    g2d_invalidate_caches();
    return g2d_vc4_launch(code_p, _unif_p, nq, srqcs_out);
}

int v3d_g2d_run_vc4(const uint64_t *code, int nwords,
                    const uint32_t *unifs, int num_qpus,
                    const void *src, size_t src_len,
                    void *dst, size_t dst_len)
{
    uint32_t nq = (uint32_t)num_qpus;
    uint32_t i, q, run_slot, span = 0, srqcs = 0;
    uint64_t *run_code;
    uint32_t run_code_p;
    int kern = -1;

    if (code == NULL || nwords <= 0 || nwords > CSD_CODE_WORDS ||
        unifs == NULL || nq == 0 || nq > 16 || !_ok || _ver != 21)
        return -1;

    /* select the preloaded VC4 kernel staging */
    if (code == g2d_qpu_argb_fill_vc4)
        kern = KERN_FILL_VC4;
    else if (code == g2d_qpu_argb_fill_loop_vc4)
        kern = KERN_FILL_LOOP_VC4;
    else if (code == g2d_qpu_argb_blit_vc4)
        kern = KERN_BLIT_VC4;
    else if (code == g2d_qpu_argb_alpha_vc4)
        kern = KERN_ALPHA_VC4;
    else if (code == g2d_qpu_argb_rotate_vc4)
        kern = KERN_ROTATE_VC4;
    else if (code == g2d_qpu_argb_gather_vc4)
        kern = KERN_GATHER_VC4;
    else if (code == g2d_qpu_argb_alpha_gather_vc4)
        kern = KERN_ALPHA_GATHER_VC4;
    else if (code == g2d_qpu_argb_copy_loop_vc4)
        kern = KERN_COPY_LOOP_VC4;
    else if (code == g2d_qpu_argb_alpha_loop_vc4)
        kern = KERN_ALPHA_LOOP_VC4;
    else
        return -1;      /* only the four bsp_g2d kernels are supported */
    if ((uint32_t)nwords > _ksrc_n[kern])
        return -1;

    if (kern == KERN_ALPHA_VC4) {
        span = unifs[4];
        if (span == 0 || span > VC4_ALPHA_SPANS)
            return -1;
        run_slot = 2u + span - 1u;
    } else if (kern > KERN_ALPHA_VC4) {
        run_slot = (uint32_t)(kern - KERN_FILL_VC4) +
                   VC4_ALPHA_SPANS - 1u;
    } else {
        run_slot = (uint32_t)(kern - KERN_FILL_VC4);
    }

    /* make the caller's ARM-side writes visible to the GPU, and drop
     * the ARM's stale copies of the destination (same contract as the
     * CSD path). */
    if (src && src_len)
        g2d_dsb();
    if (dst && dst_len)
        g2d_dsb();

    /* Allocate a fresh uniform block from the reusable staging ring.  The
     * ring is large enough for hundreds of dispatches; only a ring wrap
     * needs the expensive GPU L3 scrub. */
    uint32_t ubp;
    uint32_t *ub = (uint32_t *)v3d_g2d_vc4_staging_alloc(
                    (size_t)nq * VC4_UNIF_QWORDS * 4u, &ubp);
    if (!ub || !ubp)
        return -1;
    for (q = 0; q < nq; q++)
        for (i = 0; i < VC4_UNIF_QWORDS; i++)
            ub[q * VC4_UNIF_QWORDS + i] =
                unifs[q * VC4_UNIF_QWORDS + i];
    g2d_dsb();

    /* Pi3 bring-up fix: the init-time _kcode staging region is not
     * reliably fetchable by the VC4 QPUs on BCM2837 (the QPU reads
     * stale boot-pattern data there; the SRQ wedges).  Re-stage into the
     * selected kernel's fixed slot in the proven _run_code region. */
    if (_run_code == 0)
        return -1;
    run_code = _run_code + run_slot * CSD_CODE_WORDS;
    run_code_p = _run_code_p + run_slot * CSD_CODE_WORDS * 8u;
    int slot_new = !_vc4_slot_ready[run_slot];
    if (slot_new) {
        /* one line per kernel slot per boot: which kernels actually
         * reach the SRQ (the API test's early ops never log a
         * kern=4/kern=5 timeout, so log the first submission of each
         * kernel here to separate pre-SRQ rejection from dispatch
         * failure). */
        slog("g2d: run_vc4 first kern=%u nq=%u slot=%u code_p=0x%x\r\n",
             (uint32_t)kern, nq, run_slot, run_code_p);
        for (i = 0; i < (uint32_t)_ksrc_n[kern]; i++)
            run_code[i] = _ksrc[kern][i];
        g2d_vc4_patch_vdw(run_code, (uint32_t)_ksrc_n[kern]);
    }
    if (slot_new && kern == KERN_ALPHA_VC4) {
        uint64_t setup = 0xe00248a700000000ULL |
                         (uint64_t)(0x80804100u | (span << 16));

        for (i = 0; i < (uint32_t)_ksrc_n[kern]; i++)
            if (run_code[i] == 0xe00248a780904100ULL) {
                run_code[i] = setup;
                break;
            }
        if (i == (uint32_t)_ksrc_n[kern])
            return -1;
    }
    /* QPU_SIG_PROG_END executes two delay slots.  The generated VC4 arrays
     * currently stop at `thrend`; without mapped nop slots the first SRQ
     * increments done but leaves the QPUs unable to accept another user
     * program.  Keep the staging contract correct independent of assembler
     * output until every source kernel carries the slots explicitly. */
    run_code[_ksrc_n[kern] + 0u] = 0x100009e7009e7000ULL;
    run_code[_ksrc_n[kern] + 1u] = 0x100009e7009e7000ULL;

    /* The V3D caches do not snoop CPU writes.  Invalidate only after both
     * uniform and code staging are complete; doing this before copying the
     * kernel leaves the just-written instructions invisible to a cold SRQ
     * launch on BCM2837. */
    {
        uint32_t write_addr;

        /* The destination uniform is not at a fixed index: each kernel
         * family puts dst_row0 / dst span address somewhere different.
         * Reading the wrong word would derive flush_writes from a colour
         * or a map constant and be right only by accident. */
        if (kern == KERN_FILL_VC4 || kern == KERN_FILL_LOOP_VC4)
            write_addr = unifs[0];
        else
            write_addr = unifs[1];
        int flush_writes = (write_addr & 0xc0000000u) != 0xc0000000u;

        _vc4_dispatch_seq++;
        if (g2d_vc4_launch_mode(run_code_p, ubp, nq, &srqcs,
                                flush_writes, 1) == 0) {
            _vc4_slot_ready[run_slot] = 1;
            _vc4_need_invalidate = 0;
            /* Direct writes are already in DRAM. Cached staging writes
             * were cleaned by g2d_vc4_launch_mode(). */
            if (dst && dst_len)
                g2d_dsb();
            return 0;
        }
    }

    /* A timed-out QPU can remain live even after the counters are cleared.
     * Disable the backend so later clients fail immediately instead of
     * repeatedly filling the wedged SRQ and stalling the whole service. */
    slog("g2d: VC4 dispatch failed seq=%u kern=%u nq=%u SRQCS=0x%x "
         "unif_p=0x%x u0=0x%x u1=0x%x wraps=%u\r\n",
         _vc4_dispatch_seq, (uint32_t)kern, nq, srqcs, ubp,
         unifs[0], unifs[1], _vc4_staging_wraps);
    slog("g2d: VC4 disabled after dispatch failure; subsequent calls "
         "return -1\r\n");
    /* A timeout is a real failure.  Do not replay this possibly-live
     * operation. */
    return 1;
}
