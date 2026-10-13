/*
 * v3d_3d.c - VideoCore VII (V3D 7.1) 3D-pipeline back end for the EwokOS
 * raspberry-pi5 videocore library.  See v3d_3d.h for the design rationale.
 *
 * Ported from the bare-metal bring-up in videocore7-exps/app/src/v3d.c
 * (CLE binning thread CT0 + render thread CT1, PTB tile allocator, GL
 * shader-state records, embedded QPU coordinate/vertex/fragment shaders),
 * adapted to the EwokOS conventions established by v3d_g2d.c's CSD path:
 *
 *   - the register window, the V3D MMU (identity over sys_dma + shm_contig),
 *     the SMS power-up, the L2C enable and the clock are all brought up once
 *     by v3d_g2d_init(); this module NEVER remaps the block and NEVER issues
 *     a runtime power-domain write (PM GRAFX_V3D / SMS), which is proven to
 *     wedge the g2dd daemon.  It reaches the block only through the
 *     v3d_g2d_hub_regs()/core_regs() accessors and only once v3d_g2d_ready();
 *   - every buffer handed to the GPU (control list, binning pool, tile-state
 *     array, shader-state record + attribute records, staged QPU code,
 *     uniforms, generic tile list) is dma_alloc'ed, so it already sits inside
 *     the V3D page table and its GPU IOVA is the low 32 bits of its physical
 *     address.  The render target and the vertex attribute buffers arrive as
 *     caller-supplied IOVAs of dma / contig-shm memory;
 *   - dispatches are serialized against the CSD path with
 *     v3d_g2d_op_begin()/op_end() (a parking pthread mutex, safe to hold
 *     across the GPU wait) and share its L2 maintenance helpers;
 *   - every hardware wait is bounded by a wall-clock deadline
 *     (kernel_tic_ms), never by a fixed spin count, and a submitted job is
 *     NEVER replayed: a timeout degrades to the caller's CPU path.
 *
 * Attribute-wiring fix (left incomplete upstream): for the non-indexed
 * VERTEX_ARRAY_PRIMS path the two 16-byte vertex-attribute records must lie
 * CONTIGUOUSLY immediately after the 32-byte GL shader-state record, because
 * the CLE walks `number_of_attribute_arrays` records from
 * shader_record_address + 32.  Upstream filled a separate, never-referenced
 * buffer; here the record and its attribute arrays are one 32-byte-aligned,
 * page-based allocation (_ssr).
 *
 * All UART debug spam, the bisect/NaN/attr scaffolding, the CSD/2D parts
 * (already in v3d_g2d.c) and the watchdog/PM-reset routines are dropped.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/dma.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include "v3d_g2d.h"
#include "v3d_3d.h"

/* ---- core0 CLE / PTB register offsets (Linux drm/v3d, V3D 7.x) --------- */
#define CLE_CT0QMA   0x170u   /* bin queue message (tile-alloc pool) address */
#define CLE_CT0QMS   0x174u   /* bin queue message size                      */
#define CLE_CT0QTS   0x15cu   /* bin queue tile-state address | enable       */
#define CLE_CT0QBA   0x160u   /* bin queue base address                      */
#define CLE_CT0QEA   0x168u   /* bin queue end address (write starts the job)*/
#define CLE_CT1QBA   0x164u   /* render queue base address                   */
#define CLE_CT1QEA   0x16cu   /* render queue end address                    */
#define CLE_CT1CFG   0x178u   /* render queue config                         */
#define CLE_BFC      0x134u   /* binning flush count                         */
#define CLE_RFC      0x138u   /* render flush count                          */
#define PTB_BPOS     0x30cu   /* binning pool overspill size (kernel zeroes) */

/* ---- staging sizes ---------------------------------------------------- */
#define V3D_3D_CL_MAX      (64u * 1024u)   /* control-list buffer           */
#define V3D_3D_POOL_SIZE   (256u * 1024u)  /* binning tile-alloc pool       */
#define V3D_3D_TSDA_SIZE   (64u * 1024u)   /* tile-state data array         */
#define V3D_3D_SSR_SIZE    64u             /* 32 shader-rec + 2*16 attr-rec */
#define V3D_3D_SHADER_WORDS 128u           /* staged frag+vtx+coord + tex frag/vtx QPU */
#define V3D_3D_UNIF_WORDS  8u              /* vtx/frag uniforms             */
#define V3D_3D_GEN_SIZE    256u            /* generic tile list             */

/* Wall-clock bound for a single bin or render flush wait (ms).  The CSD
 * path uses ~256 ms; a full-frame 3D render is given more head-room, but
 * still finite so a wedged pipeline degrades instead of hanging g2dd. */
#define V3D_3D_WAIT_MS     1000u

/* ---- staged QPU code (V3D 7.1 encodings, verbatim from v3d.c) ----------
 * The QPU does not fetch code from .rodata reliably on this silicon, so the
 * three shaders are copied into dma staging at init and referenced by IOVA.
 *
 * Fragment shader: four ldvary (R,G,B,A) then two f16 vfpack writes to the
 * tile buffer, then thrsw. */
static const uint64_t _frag_shader[] = {
    0x39003186BB03F000ull, /* nop            ; ldvary rf0   (varying 0 = R) */
    0x39007186BB03F000ull, /* nop            ; ldvary rf1   (varying 1 = G) */
    0x3900B186BB03F000ull, /* nop            ; ldvary rf2   (varying 2 = B) */
    0x3900F186BB03F000ull, /* nop            ; ldvary rf3   (varying 3 = A) */
    0x380031873903F001ull, /* vfpack tlb,rf0,rf1 ; nop      (R,G -> TLB)    */
    0x380031873903F083ull, /* vfpack tlb,rf2,rf3 ; nop      (B,A -> TLB)    */
    0x38203186BB03F000ull, /* nop            ; thrsw                        */
};

/* Vertex shader: viewport transform + pass-through of the rgba attribute.
 * VPM input in[0..2]=pos.xyz, in[3..6]=rgba.  Uniforms [0]=w [1]=xscale
 * [2]=yscale [3]=zscale [4]=zoff.  VPM output out[0]=int(X*xscale)
 * out[1]=int(Y*yscale) out[2]=Z*zscale+zoff out[3]=w out[4..7]=rgba. */
static const uint64_t _vtx_shader[] = {
    0x39C02180BC03F000ull, /* ldvpmv_in rf0,0        ; nop */
    0x39C02181BC03F040ull, /* ldvpmv_in rf1,1        ; nop */
    0x39C02182BC03F080ull, /* ldvpmv_in rf2,2        ; nop */
    0x39C02183BC03F0C0ull, /* ldvpmv_in rf3,3        ; nop */
    0x3981F186BB03F000ull, /* nop                    ; ldunifrf rf7 (w) */
    0x39C02184BC03F100ull, /* ldvpmv_in rf4,4        ; nop */
    0x39823186BB03F000ull, /* nop                    ; ldunifrf rf8 (xscale) */
    0x39C02185BC03F140ull, /* ldvpmv_in rf5,5        ; nop */
    0x39827186BB03F000ull, /* nop                    ; ldunifrf rf9 (yscale) */
    0x39C02186BC03F180ull, /* ldvpmv_in rf6,6        ; nop */
    0x3982B186BB03F000ull, /* nop                    ; ldunifrf rf10 (zscale) */
    0x40001306BB008000ull, /* nop                    ; fmul rf12,rf0,rf8 */
    0x3982F186BB03F000ull, /* nop                    ; ldunifrf rf11 (zoff) */
    0x40001346BB049000ull, /* nop                    ; fmul rf13,rf1,rf9 */
    0x3800218EF503F307ull, /* ftoin rf14,rf12        ; nop (Xs) */
    0x400013C6BB08A000ull, /* nop                    ; fmul rf15,rf2,rf10 */
    0x38002190F503F347ull, /* ftoin rf16,rf13        ; nop (Ys) */
    0x380021910503F3CBull, /* fadd rf17,rf15,rf11    ; nop (Zs) */
    0x39C02180BE03F00Eull, /* stvpmv 0,rf14          ; nop (Xs) */
    0x39C02180BE03F050ull, /* stvpmv 1,rf16          ; nop (Ys) */
    0x39C02180BE03F091ull, /* stvpmv 2,rf17          ; nop (Zs) */
    0x39C02180BE03F0C7ull, /* stvpmv 3,rf7           ; nop (w) */
    0x39C02180BE03F103ull, /* stvpmv 4,rf3           ; nop (R) */
    0x39C02180BE03F144ull, /* stvpmv 5,rf4           ; nop (G) */
    0x39C02180BE03F185ull, /* stvpmv 6,rf5           ; nop (B) */
    0x39C02180BE03F1C6ull, /* stvpmv 7,rf6           ; nop (A) */
    0x38203186BB03F000ull, /* nop                    ; thrsw */
};

/* Coordinate shader: the binner's per-vertex position shader.  VPM input
 * in[0..2]=pos.xyz.  Uniforms [0]=W(1.0) [1]=xscale [2]=yscale.  VPM output
 * out[0]=X out[1]=Y out[2]=Z out[3]=W out[4]=int(X*xscale) out[5]=int(Y*yscale). */
static const uint64_t _coord_shader[] = {
    0x39C02180BC03F000ull, /* ldvpmv_in rf0,0        ; nop (X) */
    0x39C02181BC03F040ull, /* ldvpmv_in rf1,1        ; nop (Y) */
    0x39C02182BC03F080ull, /* ldvpmv_in rf2,2        ; nop (Z) */
    0x3980F186BB03F000ull, /* nop                    ; ldunifrf rf3 (W) */
    0x39813186BB03F000ull, /* nop                    ; ldunifrf rf4 (xscale) */
    0x39C02180BE03F000ull, /* stvpmv 0,rf0           ; nop (X) */
    0x39C02180BE03F041ull, /* stvpmv 1,rf1           ; nop (Y) */
    0x39817186BB03F000ull, /* nop                    ; ldunifrf rf5 (yscale) */
    0x39C02180BE03F082ull, /* stvpmv 2,rf2           ; nop (Z) */
    0x39C02180BE03F0C3ull, /* stvpmv 3,rf3           ; nop (W) */
    0x40001186BB004000ull, /* nop                    ; fmul rf6,rf0,rf4 */
    0x38002187F503F187ull, /* ftoin rf7,rf6          ; nop (Xs) */
    0x40001206BB045000ull, /* nop                    ; fmul rf8,rf1,rf5 */
    0x39C02180BE03F107ull, /* stvpmv 4,rf7           ; nop (Xs) */
    0x38002189F503F207ull, /* ftoin rf9,rf8          ; nop (Ys) */
    0x39C02180BE03F149ull, /* stvpmv 5,rf9           ; nop (Ys) */
    0x38203186BB03F000ull, /* nop                    ; thrsw */
};

/* ---- Phase D textured shaders -----------------------------------------
 * Point-sampled CLAMP-to-edge RGBA8 texture path (the 3ddemo opt-in).
 * Both arrays are host-assembled with the V3D 7.1 qpuasm dialect and every
 * word was Mesa-unpack verified field-by-field (see .tmp/videocore7-exps/
 * g2d/kernels/pgl_frag_tex.qpu and the reference vtx-shader derivation).
 * ldvary / ldunifrf / ldtmu all encode the destination register in the
 * signal-address field (verified by decoding the reference _frag_shader
 * words 0x39003186BB03F000 -> sig_addr=0 .. 0x3900F186BB03F000 -> sig_addr=3).
 *
 * Fragment-shader uniform ABI (successive ldunifrf pulls):
 *   fu0 = tex_base (IOVA of the linear RGBA8 image)
 *   fu1 = tex_stride (W * 4 bytes, integer)
 *   fu2 = (W - 1) as float   (uv * fu2 -> integer texel)
 *   fu3 = (H - 1) as float
 *   fu4 = 1 / 255.0f         (0x3B808081)
 *   fu5 = 0x000000FF         (byte mask; too big for a small immediate)
 *   fu6 = (W - 1) as int     (clamp bound for the integer texel x)
 *   fu7 = (H - 1) as int     (clamp bound for the integer texel y)
 *
 * Algorithm: ldvary rf0=u,rf1=v; fmul(u,fu2)/ftoin -> iu, fmul(v,fu3)/ftoin
 * -> iv; max/min clamp to [0,W-1]/[0,H-1]; smul24(iv,stride) + base +
 * (iu << 2); mov tmua, rf9; nop; ldtmu rf11; and/shr byte unpack to
 * R,G,B,A; itof + fmul(1/255) each; vfpack tlb (R,G) then (B,A); thrsw.
 */
static const uint64_t _frag_shader_tex[] = {
    0x39000000bb03f000ull, /* nop            ; ldvary rf0   (u)              */
    0x39004000bb03f000ull, /* nop            ; ldvary rf1   (v)              */
    0x39808000bb03f000ull, /* nop            ; ldunifrf rf2 (tex_base)       */
    0x3980c000bb03f000ull, /* nop            ; ldunifrf rf3 (tex_stride)     */
    0x39810000bb03f000ull, /* nop            ; ldunifrf rf4 (W-1 float)      */
    0x39814000bb03f000ull, /* nop            ; ldunifrf rf5 (H-1 float)      */
    0x39818000bb03f000ull, /* nop            ; ldunifrf rf6 (1/255)          */
    0x3987c000bb03f000ull, /* nop            ; ldunifrf rf31 (0xFF mask)     */
    0x39844000bb03f000ull, /* nop            ; ldunifrf rf17 (W-1 int)       */
    0x39848000bb03f000ull, /* nop            ; ldunifrf rf18 (H-1 int)       */
    0x540001c0bb004000ull, /* nop            ; fmul rf7, rf0, rf4  (u*Wm1)   */
    0x38000007f503f1c7ull, /* ftoin rf7, rf7 ; nop                  (iu)     */
    0x54000200bb045000ull, /* nop            ; fmul rf8, rf1, rf5  (v*Hm1)   */
    0x38000008f503f207ull, /* ftoin rf8, rf8 ; nop                  (iv)     */
    0x39e000077903f1c0ull, /* max rf7, rf7, 0    ; nop  (clamp iu >= 0)      */
    0x380000077803f1d1ull, /* min rf7, rf7, rf17 ; nop  (clamp iu <= W-1)    */
    0x39e000087903f200ull, /* max rf8, rf8, 0    ; nop  (clamp iv >= 0)      */
    0x380000087803f212ull, /* min rf8, rf8, rf18 ; nop  (clamp iv <= H-1)    */
    0x24000240bb203000ull, /* nop            ; smul24 rf9, rf8, rf3 (iv*str) */
    0x380000093803f242ull, /* add rf9, rf9, rf2  ; nop  (+ base)             */
    0x39e0000a7c03f1c2ull, /* shl rf10, rf7, 2   ; nop  (iu * 4)             */
    0x380000093803f24aull, /* add rf9, rf9, rf10 ; nop  (+ col byte offset)  */
    0x3800100cf903f243ull, /* mov tmua, rf9      ; nop  (TMU address)        */
    0x38000000bb03f000ull, /* nop            ; nop                            */
    0x3882c000bb03f000ull, /* nop            ; ldtmu rf11 (RGBA8 word)        */
    0x3800000cb503f2dfull, /* and rf12, rf11, rf31 ; nop   (R)                */
    0x39e000107d03f2c8ull, /* shr rf16, rf11, 8    ; nop                      */
    0x3800000db503f41full, /* and rf13, rf16, rf31 ; nop   (G)                */
    0x39e000107d03f408ull, /* shr rf16, rf16, 8    ; nop                      */
    0x3800000eb503f41full, /* and rf14, rf16, rf31 ; nop   (B)                */
    0x39e000107d03f408ull, /* shr rf16, rf16, 8    ; nop                      */
    0x3800000fb503f41full, /* and rf15, rf16, rf31 ; nop   (A)                */
    0x3800000cf603f320ull, /* itof rf12, rf12      ; nop   (R -> float)       */
    0x54000300bb306000ull, /* nop            ; fmul rf12, rf12, rf6 (R/255)  */
    0x3800000df603f360ull, /* itof rf13, rf13      ; nop   (G -> float)       */
    0x54000340bb346000ull, /* nop            ; fmul rf13, rf13, rf6 (G/255)  */
    0x3800000ef603f3a0ull, /* itof rf14, rf14      ; nop   (B -> float)       */
    0x54000380bb386000ull, /* nop            ; fmul rf14, rf14, rf6 (B/255)  */
    0x3800000ff603f3e0ull, /* itof rf15, rf15      ; nop   (A -> float)       */
    0x540003c0bb3c6000ull, /* nop            ; fmul rf15, rf15, rf6 (A/255)  */
    0x380010073503f30dull, /* vfpack tlb, rf12, rf13 ; nop  (R,G -> TLB)     */
    0x380010073503f38full, /* vfpack tlb, rf14, rf15 ; nop  (B,A -> TLB)     */
    0x38200000bb03f000ull, /* nop            ; thrsw                          */
};

/* Textured vertex shader: identical viewport transform as _vtx_shader but
 * emits 2 varyings (u,v) instead of 4 (R,G,B,A).  VPM input in[0..2]=pos.xyz,
 * in[3..5]=uv (float3 with the third component unused, matching the proven
 * pos attribute encoding to avoid an untested float2 vec_size/nvals record).
 * Uniforms [0]=w(1.0) [1]=xscale [2]=yscale [3]=zscale [4]=zoff.  VPM output
 * out[0]=Xs out[1]=Ys out[2]=Zs out[3]=w out[4]=u out[5]=v. */
static const uint64_t _vtx_shader_tex[] = {
    0x39C02180BC03F000ull, /* ldvpmv_in rf0,0        ; nop (X) */
    0x39C02181BC03F040ull, /* ldvpmv_in rf1,1        ; nop (Y) */
    0x39C02182BC03F080ull, /* ldvpmv_in rf2,2        ; nop (Z) */
    0x39C02183BC03F0C0ull, /* ldvpmv_in rf3,3        ; nop (u) */
    0x3981F186BB03F000ull, /* nop                    ; ldunifrf rf7 (w) */
    0x39C02184BC03F100ull, /* ldvpmv_in rf4,4        ; nop (v) */
    0x39823186BB03F000ull, /* nop                    ; ldunifrf rf8 (xscale) */
    0x39827186BB03F000ull, /* nop                    ; ldunifrf rf9 (yscale) */
    0x3982B186BB03F000ull, /* nop                    ; ldunifrf rf10 (zscale) */
    0x40001306BB008000ull, /* nop                    ; fmul rf12,rf0,rf8 */
    0x3982F186BB03F000ull, /* nop                    ; ldunifrf rf11 (zoff) */
    0x40001346BB049000ull, /* nop                    ; fmul rf13,rf1,rf9 */
    0x3800218EF503F307ull, /* ftoin rf14,rf12        ; nop (Xs) */
    0x400013C6BB08A000ull, /* nop                    ; fmul rf15,rf2,rf10 */
    0x38002190F503F347ull, /* ftoin rf16,rf13        ; nop (Ys) */
    0x380021910503F3CBull, /* fadd rf17,rf15,rf11    ; nop (Zs) */
    0x39C02180BE03F00Eull, /* stvpmv 0,rf14          ; nop (Xs) */
    0x39C02180BE03F050ull, /* stvpmv 1,rf16          ; nop (Ys) */
    0x39C02180BE03F091ull, /* stvpmv 2,rf17          ; nop (Zs) */
    0x39C02180BE03F0C7ull, /* stvpmv 3,rf7           ; nop (w) */
    0x39C02180BE03F103ull, /* stvpmv 4,rf3           ; nop (u varying0) */
    0x39C02180BE03F144ull, /* stvpmv 5,rf4           ; nop (v varying1) */
    0x38203186BB03F000ull, /* nop                    ; thrsw */
};

/* ---- dma staging (VA + GPU IOVA), allocated once at init -------------- */
static uint8_t  *_cl_va;      static uint32_t _cl_p;
static uint8_t  *_pool_va;    static uint32_t _pool_p;
static uint8_t  *_tsda_va;    static uint32_t _tsda_p;
static uint8_t  *_ssr_va;     static uint32_t _ssr_p;   /* shader+attr recs */
static uint64_t *_shaders_va; static uint32_t _shaders_p;
static uint32_t *_unif_va;    static uint32_t _unif_p;  /* vtx/frag uniforms */
static uint32_t *_cs_unif_va; static uint32_t _cs_unif_p;/* coord uniforms   */
static uint32_t *_tex_unif_va; static uint32_t _tex_unif_p; /* tex frag uniforms */
static uint8_t  *_gen_va;     static uint32_t _gen_p;   /* generic tile list */

/* staged shader addresses inside _shaders (IOVA), computed at init */
static uint32_t _frag_iova, _vtx_iova, _coord_iova;
static uint32_t _frag_tex_iova, _vtx_tex_iova;

static int _ready;

/* ------------------------------------------------------------------ */
/* dma staging allocation: dma_alloc is not page-aligned, so over-       */
/* allocate and round the VA up to a page (the contiguous dma mapping    */
/* keeps phys page-aligned too).  Mirrors v3d_g2d.c's MMU-table alloc.   */
/* ------------------------------------------------------------------ */
static int alloc_stage(uint32_t size, void **va, uint32_t *iova)
{
    ewokos_addr_t v = dma_alloc(0, size + 4095u);
    ewokos_addr_t p;

    if (v == 0)
        return -1;
    v = (v + 4095u) & ~(ewokos_addr_t)4095u;
    p = dma_phy_addr(0, v);
    if (p == 0 || (p >> 32) != 0)
        return -1;
    /* the buffer must be inside a RAM range present in the V3D page table */
    if (!v3d_g2d_phy_valid(p, size))
        return -1;
    *va = (void *)(uintptr_t)v;
    *iova = (uint32_t)p;
    return 0;
}

int v3d_3d_init(void)
{
    void *va;
    uint32_t iova;

    if (_ready)
        return V3D_3D_DONE;
    if (!v3d_g2d_ready())
        return V3D_3D_UNSUPPORTED;

    if (alloc_stage(V3D_3D_CL_MAX, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _cl_va = (uint8_t *)va; _cl_p = iova;

    if (alloc_stage(V3D_3D_POOL_SIZE, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _pool_va = (uint8_t *)va; _pool_p = iova;

    if (alloc_stage(V3D_3D_TSDA_SIZE, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _tsda_va = (uint8_t *)va; _tsda_p = iova;

    if (alloc_stage(V3D_3D_SSR_SIZE, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _ssr_va = (uint8_t *)va; _ssr_p = iova;

    if (alloc_stage(V3D_3D_SHADER_WORDS * 8u, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _shaders_va = (uint64_t *)va; _shaders_p = iova;

    if (alloc_stage(V3D_3D_UNIF_WORDS * 4u, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _unif_va = (uint32_t *)va; _unif_p = iova;

    if (alloc_stage(V3D_3D_UNIF_WORDS * 4u, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _cs_unif_va = (uint32_t *)va; _cs_unif_p = iova;

    if (alloc_stage(V3D_3D_UNIF_WORDS * 4u, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _tex_unif_va = (uint32_t *)va; _tex_unif_p = iova;

    if (alloc_stage(V3D_3D_GEN_SIZE, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _gen_va = (uint8_t *)va; _gen_p = iova;

    /* stage the fixed-function QPU code once; the addresses are stable for
     * the process lifetime.  frag @ [0], vtx @ [nf], coord @ [nf+nv],
     * frag_tex @ [nf+nv+nc], vtx_tex @ [nf+nv+nc+nft]. */
    {
        uint32_t nf  = sizeof(_frag_shader) / 8u;
        uint32_t nv  = sizeof(_vtx_shader) / 8u;
        uint32_t nc  = sizeof(_coord_shader) / 8u;
        uint32_t nft = sizeof(_frag_shader_tex) / 8u;
        uint32_t nvt = sizeof(_vtx_shader_tex) / 8u;
        uint32_t i;

        if (nf + nv + nc + nft + nvt > V3D_3D_SHADER_WORDS)
            return V3D_3D_UNSUPPORTED;
        for (i = 0; i < nf; i++)  _shaders_va[i] = _frag_shader[i];
        for (i = 0; i < nv; i++)  _shaders_va[nf + i] = _vtx_shader[i];
        for (i = 0; i < nc; i++)  _shaders_va[nf + nv + i] = _coord_shader[i];
        for (i = 0; i < nft; i++) _shaders_va[nf + nv + nc + i] = _frag_shader_tex[i];
        for (i = 0; i < nvt; i++) _shaders_va[nf + nv + nc + nft + i] = _vtx_shader_tex[i];
        _frag_iova     = _shaders_p;
        _vtx_iova      = _shaders_p + nf * 8u;
        _coord_iova    = _shaders_p + (nf + nv) * 8u;
        _frag_tex_iova = _shaders_p + (nf + nv + nc) * 8u;
        _vtx_tex_iova  = _shaders_p + (nf + nv + nc + nft) * 8u;
    }
    __asm__ __volatile__("dsb sy" ::: "memory");

    _ready = 1;
    klog("v3d_3d: 3D pipeline staging ready (cl %u pool %u tsda %u)\n",
         _cl_p, _pool_p, _tsda_p);
    return V3D_3D_DONE;
}

int v3d_3d_ready(void)
{
    return _ready;
}

/* ------------------------------------------------------------------ */
/* control-list byte writers (index into the dma _cl buffer, which is    */
/* NOCACHE: no dcache maintenance, but a dsb orders the writes before    */
/* the doorbell).  Bounds-guarded; overflow makes the length exceed       */
/* V3D_3D_CL_MAX and the caller declines.                                */
/* ------------------------------------------------------------------ */
static uint32_t cl_u8(uint32_t off, uint8_t v)
{
    if (off < V3D_3D_CL_MAX)
        _cl_va[off] = v;
    return off + 1;
}

static uint32_t cl_u16le(uint32_t off, uint32_t v)
{
    if (off + 1 < V3D_3D_CL_MAX) {
        _cl_va[off]     = (uint8_t)(v & 0xFF);
        _cl_va[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    }
    return off + 2;
}

static uint32_t cl_u32le(uint32_t off, uint32_t v)
{
    if (off + 3 < V3D_3D_CL_MAX) {
        _cl_va[off]     = (uint8_t)(v & 0xFF);
        _cl_va[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        _cl_va[off + 2] = (uint8_t)((v >> 16) & 0xFF);
        _cl_va[off + 3] = (uint8_t)((v >> 24) & 0xFF);
    }
    return off + 4;
}

static uint32_t cl_f32le(uint32_t off, float v)
{
    uint32_t u;
    __builtin_memcpy(&u, &v, 4);
    return cl_u32le(off, u);
}

/* Convert an IEEE-754 float32 to the V3D "f187" half-float used by the Depth
 * Offset packet's Factor/Units fields (1 sign, 8 exponent bias-127, 7
 * mantissa).  Because the exponent width and bias match float32, f187 is the
 * top 16 bits of the float32 with the mantissa truncated to 7 bits.  Best
 * effort: exact rounding / denormal handling is confirmed on the Pi5. */
static uint16_t cl_f187(float v)
{
    uint32_t u;
    __builtin_memcpy(&u, &v, 4);
    return (uint16_t)((((u >> 31) & 1u) << 15) |
                      (((u >> 23) & 0xFFu) << 7) |
                      ((u >> 16) & 0x7Fu));
}

/* ------------------------------------------------------------------ */
/* GL shader-state record (32 bytes) + the two 16-byte attribute records */
/* written CONTIGUOUSLY into the one page-based _ssr allocation.          */
/* ------------------------------------------------------------------ */
static void build_shader_record(uint32_t frag_addr, uint32_t frag_unif,
                                uint32_t vtx_addr, uint32_t vtx_unif,
                                uint32_t coord_addr, uint32_t coord_unif,
                                uint32_t varyings)
{
    uint8_t *r = _ssr_va;                 /* bytes [0..31] */

    memset(r, 0, 32);
    r[0] = (1u << 1);                     /* enable_clipping */
    r[1] = (1u << 4);                     /* real_pixel_centre_w */
    r[2] = (1u << 2);                     /* disable_implicit_point_line_varyings */
    r[3] = (uint8_t)(varyings & 0x1Fu);   /* varyings into fragment shader (4 gouraud, 2 tex) */
    /* VPM segment sizes (7.1): coord/vtx input sizes 0 (shared with output),
     * high nibbles = min segments in play (As=1, Ve=0). */
    r[4] = 0x01;                          /* coord out segs=1, Ve=0 */
    r[5] = 0x10;                          /* coord in segs=0, As=1  */
    r[6] = 0x01;                          /* vtx  out segs=1, Ve=0  */
    r[7] = 0x10;                          /* vtx  in segs=0, As=1   */
    r[8]  = (uint8_t)(frag_addr & 0xF8);
    r[9]  = (uint8_t)((frag_addr >> 8) & 0xFF);
    r[10] = (uint8_t)((frag_addr >> 16) & 0xFF);
    r[11] = (uint8_t)((frag_addr >> 24) & 0xFF);
    r[12] = (uint8_t)(frag_unif & 0xFF);
    r[13] = (uint8_t)((frag_unif >> 8) & 0xFF);
    r[14] = (uint8_t)((frag_unif >> 16) & 0xFF);
    r[15] = (uint8_t)((frag_unif >> 24) & 0xFF);
    r[16] = (uint8_t)(vtx_addr & 0xF8);
    r[17] = (uint8_t)((vtx_addr >> 8) & 0xFF);
    r[18] = (uint8_t)((vtx_addr >> 16) & 0xFF);
    r[19] = (uint8_t)((vtx_addr >> 24) & 0xFF);
    r[20] = (uint8_t)(vtx_unif & 0xFF);
    r[21] = (uint8_t)((vtx_unif >> 8) & 0xFF);
    r[22] = (uint8_t)((vtx_unif >> 16) & 0xFF);
    r[23] = (uint8_t)((vtx_unif >> 24) & 0xFF);
    r[24] = (uint8_t)(coord_addr & 0xF8);
    r[25] = (uint8_t)((coord_addr >> 8) & 0xFF);
    r[26] = (uint8_t)((coord_addr >> 16) & 0xFF);
    r[27] = (uint8_t)((coord_addr >> 24) & 0xFF);
    r[28] = (uint8_t)(coord_unif & 0xFF);
    r[29] = (uint8_t)((coord_unif >> 8) & 0xFF);
    r[30] = (uint8_t)((coord_unif >> 16) & 0xFF);
    r[31] = (uint8_t)((coord_unif >> 24) & 0xFF);
}

/* attr record 0 = pos (float3, stride 12); attr record 1 = col (byte4
 * normalized, stride 4).  Written at _ssr + 32 (contiguous after the
 * 32-byte shader record). */
static void build_attr_records(uint32_t pos_iova, uint32_t col_iova)
{
    uint8_t *a = _ssr_va + 32;            /* bytes [32..63] */

    memset(a, 0, 32);
    a[0]  = (uint8_t)(pos_iova & 0xFF);
    a[1]  = (uint8_t)((pos_iova >> 8) & 0xFF);
    a[2]  = (uint8_t)((pos_iova >> 16) & 0xFF);
    a[3]  = (uint8_t)((pos_iova >> 24) & 0xFF);
    a[4]  = (3u << 0) | (2u << 2);        /* vec_size=3, type=FLOAT */
    a[5]  = (3u << 4) | (3u << 0);        /* nvals vs=3, cs=3 */
    a[8]  = 12;                           /* stride = 12 bytes */
    a[12] = 0xFF; a[13] = 0xFF; a[14] = 0xFF; a[15] = 0xFF;  /* max index */
    a[16] = (uint8_t)(col_iova & 0xFF);
    a[17] = (uint8_t)((col_iova >> 8) & 0xFF);
    a[18] = (uint8_t)((col_iova >> 16) & 0xFF);
    a[19] = (uint8_t)((col_iova >> 24) & 0xFF);
    a[20] = (0u << 0) | (4u << 2) | (1u << 6);  /* vec_size=0, BYTE, normalized */
    a[21] = (0u << 0) | (4u << 4);              /* nvals vs=4, cs=0 */
    a[24] = 4;                            /* stride = 4 bytes */
    a[28] = 0xFF; a[29] = 0xFF; a[30] = 0xFF; a[31] = 0xFF;
}

/* TEX_MOD variant: attr 0 = pos (float3), attr 1 = uv (float3 with the third
 * component unused).  The uv record reuses the proven pos float3 encoding
 * (vec_size=3, type=FLOAT, stride 12, nvals vs=3/cs=3) so the vertex shader
 * reads in[3..5] = (u, v, pad); the third component is discarded.  This
 * avoids shipping an untested float2 vec_size/nvals combination. */
static void build_attr_records_tex(uint32_t pos_iova, uint32_t uv_iova)
{
    uint8_t *a = _ssr_va + 32;            /* bytes [32..63] */

    memset(a, 0, 32);
    a[0]  = (uint8_t)(pos_iova & 0xFF);
    a[1]  = (uint8_t)((pos_iova >> 8) & 0xFF);
    a[2]  = (uint8_t)((pos_iova >> 16) & 0xFF);
    a[3]  = (uint8_t)((pos_iova >> 24) & 0xFF);
    a[4]  = (3u << 0) | (2u << 2);        /* vec_size=3, type=FLOAT */
    a[5]  = (3u << 4) | (3u << 0);        /* nvals vs=3, cs=3 */
    a[8]  = 12;                           /* stride = 12 bytes */
    a[12] = 0xFF; a[13] = 0xFF; a[14] = 0xFF; a[15] = 0xFF;  /* max index */
    a[16] = (uint8_t)(uv_iova & 0xFF);
    a[17] = (uint8_t)((uv_iova >> 8) & 0xFF);
    a[18] = (uint8_t)((uv_iova >> 16) & 0xFF);
    a[19] = (uint8_t)((uv_iova >> 24) & 0xFF);
    a[20] = (3u << 0) | (2u << 2);        /* vec_size=3, type=FLOAT */
    a[21] = (3u << 4) | (3u << 0);        /* nvals vs=3, cs=3 */
    a[24] = 12;                           /* stride = 12 bytes */
    a[28] = 0xFF; a[29] = 0xFF; a[30] = 0xFF; a[31] = 0xFF;
}

/* ------------------------------------------------------------------ */
/* bin flush / render flush wait: wall-clock bounded (never a fixed spin */
/* count), parks with usleep so the parking mutex is not held spinning.  */
/* ------------------------------------------------------------------ */
static int wait_flush(volatile uint32_t *core, uint32_t reg, uint32_t before)
{
    uint64_t deadline = kernel_tic_ms(0) + V3D_3D_WAIT_MS;

    while (kernel_tic_ms(0) < deadline) {
        if (core[reg / 4] != before)
            return 0;
        usleep(200);
    }
    return -1;
}

/* submit the binning CL: point the pool + tile-state at the dma staging,
 * drop stale GPU cache lines, ring CT0, wait for the bin flush count. */
static int submit_bin(volatile uint32_t *core, uint32_t start, uint32_t end)
{
    uint32_t before = core[CLE_BFC / 4];

    core[CLE_CT0QMA / 4] = _pool_p;
    core[CLE_CT0QMS / 4] = V3D_3D_POOL_SIZE;
    core[CLE_CT0QTS / 4] = (1u << 1) | _tsda_p;   /* enable | page-aligned tsda */
    core[PTB_BPOS / 4]   = 0;                     /* kernel zeroes overspill */
    __asm__ __volatile__("dsb sy" ::: "memory");
    v3d_g2d_l2_invalidate_all();
    core[CLE_CT0QBA / 4] = start;
    core[CLE_CT0QEA / 4] = end;                   /* write starts the job */
    return wait_flush(core, CLE_BFC, before);
}

/* submit the render CL: drop stale cache lines, ring CT1, wait for the
 * render flush count. */
static int submit_render(volatile uint32_t *core, uint32_t start, uint32_t end)
{
    uint32_t before = core[CLE_RFC / 4];

    v3d_g2d_l2_invalidate_all();
    core[CLE_CT1CFG / 4] = 0;                     /* QCFG default */
    __asm__ __volatile__("dsb sy" ::: "memory");
    core[CLE_CT1QBA / 4] = start;
    core[CLE_CT1QEA / 4] = end;
    return wait_flush(core, CLE_RFC, before);
}

/* ------------------------------------------------------------------ */
/* empty BCL (clear path): produces the per-tile lists the RCL branches  */
/* to.  16px tiles to match build_clear_rcl.                             */
/* ------------------------------------------------------------------ */
static uint32_t build_bcl_empty(uint32_t w, uint32_t h)
{
    uint32_t off = 0;

    off = cl_u8(off, 0x77);                     /* NUMBER_OF_LAYERS */
    off = cl_u8(off, 0x00);                     /* layers - 1 = 0 */
    off = cl_u8(off, 0x78);                     /* TILE_BINNING_MODE_CFG */
    off = cl_u8(off, (1u << 4) | (0u << 2));    /* init block 128b, overflow 64b */
    off = cl_u8(off, (1u << 3) | (1u << 0));    /* log2 tile h/w = 1 (16px) */
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x00);
    off = cl_u16le(off, w - 1u);
    off = cl_u16le(off, h - 1u);
    off = cl_u8(off, 0x13);                     /* FLUSH_VCD_CACHE */
    off = cl_u8(off, 0x06);                     /* START_TILE_BINNING */
    off = cl_u8(off, 0x04);                     /* FLUSH */
    return off;
}

/* clear RCL: render-target config, an explicit global TLB clear (2x, the
 * GFXH-1742 workaround), then a per-tile store of the cleared tile buffer
 * to the framebuffer.  16px tiles. */
static uint32_t build_clear_rcl(uint32_t fb_iova, uint32_t stride_bytes,
                                uint32_t w, uint32_t h, uint32_t tile,
                                uint32_t fmt, uint32_t rgba8, int rb_swap)
{
    uint32_t off = 0;
    uint32_t tx = (w + tile - 1) / tile;
    uint32_t ty = (h + tile - 1) / tile;
    uint32_t log2t = 1;             /* 16 px tiles */
    uint32_t x, y, i;

    /* TILE_RENDERING_MODE_CFG_COMMON (0x79, sub_id 0) */
    off = cl_u8(off, 0x79);
    off = cl_u8(off, 0x00);                     /* nrt-1 = 0, sub_id = 0 */
    off = cl_u16le(off, w);
    off = cl_u16le(off, h);
    off = cl_u8(off, (1u << 6) | (1u << 4));    /* early_z_disable, depth_buf_disable */
    off = cl_u8(off, (uint8_t)((log2t << 7) | (log2t << 4)));
    off = cl_u8(off, 0x00);

    /* TILE_RENDERING_MODE_CFG_RENDER_TARGET_PART1 (sub_id 2) */
    {
        uint32_t rt_stride = (tile / 2u) - 1u;
        off = cl_u8(off, 0x79);
        off = cl_u8(off, 0x02);                 /* sub_id = 2 */
        off = cl_u8(off, 0x00);                 /* base_address bits 8-15 */
        off = cl_u8(off, (uint8_t)((rt_stride & 0x7F) << 2));
        off = cl_u8(off, (uint8_t)((rt_stride >> 6) | (8u << 3) | (0u << 1)));
        off = cl_u32le(off, rgba8);             /* clear color low */
    }

    /* TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES (sub_id 1): z=1.0, stencil=0 */
    off = cl_u8(off, 0x79);
    off = cl_u8(off, 0x01);
    off = cl_u8(off, 0x00);
    off = cl_u32le(off, 0x3F800000u);           /* z = 1.0f */
    off = cl_u16le(off, 0x0000);

    /* initial TLB clear: global, 2 iterations (GFXH-1742 workaround) */
    for (i = 0; i < 2; i++) {
        off = cl_u8(off, 0x7C);                 /* TILE_COORDINATES (0,0) */
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x1A);                 /* END_OF_LOADS */
        off = cl_u8(off, 0x1D);                 /* STORE dummy (NONE) */
        off = cl_u8(off, 0x08);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u16le(off, 0x00);
        off = cl_u32le(off, 0x00);
        off = cl_u8(off, 0x19);                 /* CLEAR_RENDER_TARGETS */
        off = cl_u8(off, 0x1B);                 /* END_OF_TILE_MARKER */
    }

    off = cl_u8(off, 0x13);                     /* FLUSH_VCD_CACHE */

    /* per-tile store of the (cleared) tile buffer to the framebuffer */
    for (y = 0; y < ty; y++) {
        for (x = 0; x < tx; x++) {
            off = cl_u8(off, 0x7C);             /* TILE_COORDINATES */
            off = cl_u8(off, (uint8_t)(x & 0xFF));
            off = cl_u8(off, (uint8_t)(((y & 0x0F) << 4) | ((x >> 8) & 0x0F)));
            off = cl_u8(off, (uint8_t)((y >> 4) & 0xFF));
            off = cl_u8(off, 0x1A);             /* END_OF_LOADS */
            off = cl_u8(off, 0x1D);             /* STORE_TILE_BUFFER_GENERAL */
            off = cl_u8(off, 0x00);             /* RASTER, RT0 */
            off = cl_u8(off, (uint8_t)((fmt & 0x0F) << 4));
            off = cl_u8(off, (uint8_t)((rb_swap ? 0x10u : 0x00u) |
                                       ((fmt >> 4) & 0x03u)));
            off = cl_u8(off, (uint8_t)((stride_bytes & 0x0Fu) << 4));
            off = cl_u8(off, (uint8_t)((stride_bytes >> 4) & 0xFF));
            off = cl_u8(off, (uint8_t)((stride_bytes >> 12) & 0xFF));
            off = cl_u16le(off, 0);             /* height */
            off = cl_u32le(off, fb_iova);       /* absolute address */
            off = cl_u8(off, 0x1B);             /* END_OF_TILE_MARKER */
        }
    }

    off = cl_u8(off, 0x0D);                     /* END_OF_RENDERING */
    return off;
}

/* Map a V3D_3D_DEPTH_* function code to the V3D hardware depth_test_function
 * field (3-bit Compare Function enum: 0 never, 1 less, 2 equal, 3 lequal,
 * 4 greater, 5 notequal, 6 gequal, 7 always).  All eight GL compare functions
 * are covered.  NONE (test disabled) maps to 0 and is never combined with the
 * Z buffer, so its value is don't-care. */
static uint32_t depth_func_hw(uint32_t depth_func)
{
    switch (depth_func) {
    case V3D_3D_DEPTH_NEVER:    return 0u;
    case V3D_3D_DEPTH_LESS:     return 1u;
    case V3D_3D_DEPTH_EQUAL:    return 2u;
    case V3D_3D_DEPTH_LEQUAL:   return 3u;
    case V3D_3D_DEPTH_GREATER:  return 4u;
    case V3D_3D_DEPTH_NOTEQUAL: return 5u;
    case V3D_3D_DEPTH_GEQUAL:   return 6u;
    case V3D_3D_DEPTH_ALWAYS:   return 7u;
    default:                    return 0u;   /* NONE (test disabled) */
    }
}

/* ------------------------------------------------------------------ */
/* draw BCL: full render state + shader state + a real primitive list.   */
/* 64px tiles (log2t=3) to match build_draw_rcl and to keep the 64 KiB     */
/* tile-state buffer from overflowing.                                     */
/* ------------------------------------------------------------------ */
static uint32_t build_draw_bcl(int mode, uint32_t nverts, uint32_t sr_iova,
                               uint32_t nattrs, uint32_t w, uint32_t h,
                               uint32_t depth_func, uint32_t depth_mask,
                               uint32_t cull_en, const v3d_3d_ext_t *ext)
{
    uint32_t off = 0;
    uint32_t log2t = 3;
    uint32_t half_x = w / 2u, half_y = h / 2u;

    off = cl_u8(off, 0x77);                     /* NUMBER_OF_LAYERS */
    off = cl_u8(off, 0x00);

    off = cl_u8(off, 0x78);                     /* TILE_BINNING_MODE_CFG */
    off = cl_u8(off, (0u << 4) | (1u << 2));    /* init 128B, overflow 64B */
    off = cl_u8(off, (uint8_t)((log2t << 3) | log2t));
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x00);
    off = cl_u16le(off, w - 1);
    off = cl_u16le(off, h - 1);
    off = cl_u8(off, 0x13);                     /* FLUSH_VCD_CACHE */
    off = cl_u8(off, 0x5C);                     /* OCCLUSION_QUERY_COUNTER(0) */
    off = cl_u32le(off, 0);
    off = cl_u8(off, 0x06);                     /* START_TILE_BINNING */
    if (ext && ext->scissor_w > 0 && ext->scissor_h > 0) {
        off = cl_u8(off, 0x6B);                 /* CLIP_WINDOW = scissor rect.
                                                 * Left/Bottom/Width/Height, the
                                                 * bottom already y-down from the
                                                 * top edge (client flipped it). */
        off = cl_u16le(off, (uint32_t)ext->scissor_x);
        off = cl_u16le(off, (uint32_t)ext->scissor_y);
        off = cl_u16le(off, (uint32_t)ext->scissor_w);
        off = cl_u16le(off, (uint32_t)ext->scissor_h);
    } else {
        off = cl_u8(off, 0x6B);                 /* CLIP_WINDOW 0,0,w,h */
        off = cl_u16le(off, 0);
        off = cl_u16le(off, 0);
        off = cl_u16le(off, w);
        off = cl_u16le(off, h);
    }
    {   /* CFG_BITS (0x60), V3D 7.1 field placement (mesa v3d_packet.xml):
         * byte0 bit0 = enable forward facing, bit1 = enable reverse facing,
         * bit2 = clockwise primitives; byte1 bits 4-6 (= packet bits 12-14) =
         * Depth-Test Function (Compare Function enum), bit7 (= packet bit 15)
         * = Z updates enable.  V3D 7.1 has NO separate depth-test-enable bit:
         * the test runs whenever a depth buffer is allocated (TRM_CFG_COMMON
         * depth_buf_disable cleared) and the function is not NEVER.  cull_en
         * carries a V3D_3D_CULL_* config code selecting which facing(s) survive
         * plus the front-face winding; CULL_NONE keeps both facings (draw
         * everything).  With depth_func == NONE and cull_en == CULL_NONE the
         * bytes equal the historical depth/cull-off base. */
        uint8_t b0;
        uint8_t b1 = 0x00;
        /* NOTE: the render target is y-down (the vtx shader scales y by -h/2),
         * so if the Pi5 shows the winding inverted, flip the clockwise bit. */
        switch (cull_en) {
        case V3D_3D_CULL_BACK_CCW:  b0 = 0x01; break;  /* forward only, ccw */
        case V3D_3D_CULL_BACK_CW:   b0 = 0x05; break;  /* forward only, cw  */
        case V3D_3D_CULL_FRONT_CCW: b0 = 0x02; break;  /* reverse only, ccw */
        case V3D_3D_CULL_FRONT_CW:  b0 = 0x06; break;  /* reverse only, cw  */
        case V3D_3D_CULL_BOTH:      b0 = 0x00; break;  /* cull everything   */
        default:                    b0 = 0x03; break;  /* no cull: both     */
        }
        if (ext && ext->poly_enable)
            b0 |= 0x08;                 /* enable depth offset, CFG_BITS bit3 */
        if (depth_func != V3D_3D_DEPTH_NONE) {
            b1 |= (uint8_t)(depth_func_hw(depth_func) << 4); /* func bits 12-14 */
            if (depth_mask)
                b1 |= 0x80;                 /* Z updates enable, bit 15 */
        }
        off = cl_u8(off, 0x60);
        off = cl_u8(off, b0);
        off = cl_u8(off, b1);
        off = cl_u8(off, 0x00);
    }
    if (ext && ext->poly_enable) {          /* DEPTH_OFFSET (0x6A): Factor (f187,
                                             * bytes0-1), Units (f187, bytes2-3),
                                             * Limit (float32, bytes4-7, unused). */
        off = cl_u8(off, 0x6A);
        off = cl_u16le(off, cl_f187(ext->poly_factor));
        off = cl_u16le(off, cl_f187(ext->poly_units));
        off = cl_f32le(off, 0.0f);
    }
    off = cl_u8(off, 0x68);                     /* POINT_SIZE */
    off = cl_f32le(off, 1.0f);
    off = cl_u8(off, 0x69);                     /* LINE_WIDTH */
    off = cl_f32le(off, 1.0f);
    off = cl_u8(off, 0x6E);                     /* CLIPPER_XY_SCALING (1/64 px) */
    off = cl_f32le(off, (float)half_x * 64.0f);
    off = cl_f32le(off, -(float)half_y * 64.0f);
    off = cl_u8(off, 0x6F);                     /* CLIPPER_Z_SCALE_AND_OFFSET */
    off = cl_f32le(off, 0.5f);
    off = cl_f32le(off, 0.5f);
    off = cl_u8(off, 0x6D);                     /* CLIPPER_Z_MIN_MAX 0..1 */
    off = cl_f32le(off, 0.0f);
    off = cl_f32le(off, 1.0f);
    {                                           /* VIEWPORT_OFFSET (14.8 fixed) */
        uint32_t fine_x = (uint32_t)((uint64_t)half_x * 256u);
        uint32_t fine_y = (uint32_t)((uint64_t)half_y * 256u);
        off = cl_u8(off, 0x6C);
        off = cl_u8(off, (uint8_t)(fine_x & 0xFF));
        off = cl_u8(off, (uint8_t)((fine_x >> 8) & 0xFF));
        off = cl_u8(off, (uint8_t)((fine_x >> 16) & 0x3F));
        off = cl_u8(off, 0x00);                 /* coarse x = 0 */
        off = cl_u8(off, (uint8_t)(fine_y & 0xFF));
        off = cl_u8(off, (uint8_t)((fine_y >> 8) & 0xFF));
        off = cl_u8(off, (uint8_t)((fine_y >> 16) & 0x3F));
        off = cl_u8(off, 0x00);                 /* coarse y = 0 */
    }
    off = cl_u8(off, 0x53);                     /* BLEND_ENABLES: none */
    off = cl_u8(off, 0x00);
    {   /* COLOR_WRITE_MASKS (0x57).  The reference Pi5 path emits 0 and still
         * renders full colour, so a CLEARED mask = write-all: a SET bit
         * DISABLES a channel.  ext->color_mask is the GL write-enable bitmask
         * (bit0=R..bit3=A); invert its low nibble so write-all (0xF) and an
         * absent/old ext (0) both yield the proven 0.  Channel order is the
         * natural GL R/G/B/A mapping, confirmed on the Pi5. */
        uint32_t wm = 0;
        if (ext && ext->color_mask != 0 && (ext->color_mask & 0xFu) != 0xFu)
            wm = (~ext->color_mask) & 0xFu;
        off = cl_u8(off, 0x57);
        off = cl_u32le(off, wm);
    }
    off = cl_u8(off, 0x56);                     /* BLEND_CONSTANT_COLOR f16 x4 */
    off = cl_u16le(off, 0);
    off = cl_u16le(off, 0);
    off = cl_u16le(off, 0);
    off = cl_u16le(off, 0);
    off = cl_u8(off, 0x61);                     /* ZERO_ALL_FLAT_SHADE */
    off = cl_u8(off, 0x63);                     /* ZERO_ALL_NON_PERSPECTIVE */
    off = cl_u8(off, 0x58);                     /* ZERO_ALL_CENTROID */
    off = cl_u8(off, 0x4A);                     /* TRANSFORM_FEEDBACK_SPECS */
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x5C);                     /* OCCLUSION_QUERY_COUNTER(0) */
    off = cl_u32le(off, 0);
    off = cl_u8(off, 0x5B);                     /* SAMPLE_STATE mask 0xF, cov 1.0 */
    off = cl_u8(off, 0x0F);
    off = cl_u8(off, 0x80);
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x47);                     /* VCM_CACHE_SIZE 4,4 */
    off = cl_u8(off, 0x44);
    off = cl_u8(off, 0x40);                     /* GL_SHADER_STATE */
    off = cl_u8(off, (uint8_t)(sr_iova & 0xFF) | (uint8_t)(nattrs & 0x1F));
    off = cl_u8(off, (uint8_t)((sr_iova >> 8) & 0xFF));
    off = cl_u8(off, (uint8_t)((sr_iova >> 16) & 0xFF));
    off = cl_u8(off, (uint8_t)((sr_iova >> 24) & 0xFF));
    off = cl_u8(off, 0x24);                     /* VERTEX_ARRAY_PRIMS */
    off = cl_u8(off, (uint8_t)mode);
    off = cl_u32le(off, nverts);
    off = cl_u32le(off, 0);                     /* first = 0 */
    off = cl_u8(off, 0x04);                     /* FLUSH */
    return off;
}

/* Map a VERTEX_ARRAY_PRIMS mode to the generic tile list's PRIM_LIST_FORMAT
 * primitive-type field (Prim List Format packet: 0 = List Points, 1 = List
 * Lines, 2 = List Triangles). */
static uint8_t prim_list_type(int mode)
{
    switch (mode) {
    case V3D_3D_MODE_POINTS: return 0u;
    case V3D_3D_MODE_LINES:  return 1u;
    default:                 return 2u;   /* triangles */
    }
}

/* Emit a STORE(0x1D)/LOAD(0x1E) Tile-Buffer-General packet (opcode + 12
 * bytes) for one buffer into the generic tile list at gp[*g].  Raster memory
 * format, no flip/dither/decimate.  buffer: 0 = RT0 colour, 9 = Z.  imgfmt is
 * an Output Image Format code (rgba8 = 27, d32f = 40).  stride is the linear
 * row pitch in bytes; addr the surface IOVA.  Field placement per mesa
 * v3d_packet.xml (Store/Load Tile Buffer General): Buffer bits 0-3, Memory
 * Format bits 4-6, Image Format bits 12-17, R/B swap bit 20, Stride bits
 * 28-47, Height bits 48-63, Address bits 64-95. */
static void gen_emit_store_load(uint8_t *gp, uint32_t *g, uint8_t opcode,
                                uint32_t buffer, uint32_t imgfmt, int rb_swap,
                                uint32_t stride, uint32_t addr)
{
    gp[(*g)++] = opcode;
    gp[(*g)++] = (uint8_t)(buffer & 0x0F);              /* memfmt raster = 0 */
    gp[(*g)++] = (uint8_t)((imgfmt & 0x0F) << 4);       /* img fmt bits 12-15 */
    gp[(*g)++] = (uint8_t)((rb_swap ? 0x10u : 0x00u) | ((imgfmt >> 4) & 0x03u));
    gp[(*g)++] = (uint8_t)((stride & 0x0Fu) << 4);      /* stride bits 28-31 */
    gp[(*g)++] = (uint8_t)((stride >> 4) & 0xFF);       /* stride bits 32-39 */
    gp[(*g)++] = (uint8_t)((stride >> 12) & 0xFF);      /* stride bits 40-47 */
    gp[(*g)++] = 0x00;                                  /* height bits 48-55 */
    gp[(*g)++] = 0x00;                                  /* height bits 56-63 */
    gp[(*g)++] = (uint8_t)(addr & 0xFF);                /* address bits 64-95 */
    gp[(*g)++] = (uint8_t)((addr >> 8) & 0xFF);
    gp[(*g)++] = (uint8_t)((addr >> 16) & 0xFF);
    gp[(*g)++] = (uint8_t)((addr >> 24) & 0xFF);
}

/* draw RCL: TRM_CFG + tile machinery + the generic tile list (implicit
 * per-tile branch to the binned primitives).  64px tiles.  Persistent depth:
 * the first dispatch of a frame (first_frame) runs the global TLB clear
 * (GFXH-1742 workaround) which initialises colour to rgba8 and Z to clear_z,
 * then stores colour (+Z when a persistent Z surface is bound); every later
 * dispatch LOADs colour (+Z) from DRAM before rendering and stores both back,
 * so the framebuffer and depth accumulate across dispatches within a frame. */
static uint32_t build_draw_rcl(uint32_t fb_iova, uint32_t stride_bytes,
                               uint32_t fmt, uint32_t rgba8, int rb_swap,
                               uint32_t w, uint32_t h, int depth_en,
                               uint32_t z_iova, uint32_t z_stride,
                               int first_frame, float clear_z, int mode)
{
    uint32_t off = 0;
    uint32_t log2t = 3;               /* 64 px tiles */
    uint32_t tx = (w + 63) / 64;
    uint32_t ty = (h + 63) / 64;
    uint32_t i;

    /* TRM_CFG_COMMON (0x79, sub 0): nrt=1, w, h, 64px tiles.  Bit 4 is
     * depth_buf_disable: it stays SET when the caller wants no depth (the
     * historical colour-only path) and is CLEARED when depth is enabled so
     * the tile-local Z buffer is allocated and used.  Internal Depth Type
     * (bits 47-50) stays 0 = depth_32f, matching the persistent Z32F surface
     * the STORE/LOAD packets below move in and out of DRAM.  early_z_disable
     * (bit 6) stays set: the fixed-function frag does not write depth, and
     * resolving Z after the fragment is the conservative choice. */
    off = cl_u8(off, 0x79);
    off = cl_u8(off, 0x00);
    off = cl_u16le(off, w);
    off = cl_u16le(off, h);
    off = cl_u8(off, depth_en ? (uint8_t)(1u << 6)
                              : (uint8_t)((1u << 6) | (1u << 4)));
    off = cl_u8(off, (uint8_t)((log2t << 7) | (log2t << 4)));
    off = cl_u8(off, (uint8_t)((log2t >> 1) & 0x03));
    /* RT_PART1 (0x79, sub 2): clear color, clamp-8, bpp-32, rt stride */
    {
        uint32_t rt_stride = (64u / 2u) - 1u;
        off = cl_u8(off, 0x79);
        off = cl_u8(off, 0x02);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, (uint8_t)((rt_stride & 0x7F) << 2));
        off = cl_u8(off, (uint8_t)((rt_stride >> 6) | (8u << 3) | (0u << 1)));
        off = cl_u32le(off, rgba8);
    }
    /* ZS_CLEAR_VALUES (0x79, sub 1): z = clear_z (float bits), stencil = 0 */
    {
        uint32_t zbits;
        __builtin_memcpy(&zbits, &clear_z, 4);
        off = cl_u8(off, 0x79);
        off = cl_u8(off, 0x01);
        off = cl_u8(off, 0x00);
        off = cl_u32le(off, zbits);
        off = cl_u16le(off, 0x0000);
    }
    /* TILE_LIST_INITIAL_BLOCK_SIZE (0x7E): auto-chain, 128b */
    off = cl_u8(off, 0x7E);
    off = cl_u8(off, (1u << 2) | (1u << 0));
    /* MULTICORE_RENDERING_SUPERTILE_CFG (0x7A): 1 supertile = whole frame */
    off = cl_u8(off, 0x7A);
    off = cl_u8(off, (uint8_t)(tx - 1));        /* supertile_w - 1 (tiles) */
    off = cl_u8(off, (uint8_t)(ty - 1));
    off = cl_u8(off, 1);                        /* frame w in supertiles */
    off = cl_u8(off, 1);
    off = cl_u8(off, (uint8_t)(tx & 0xFF));
    off = cl_u8(off, (uint8_t)(((ty & 0x0F) << 4) | ((tx >> 8) & 0x0F)));
    off = cl_u8(off, (uint8_t)((ty >> 4) & 0xFF));
    off = cl_u8(off, 0x00);                     /* nblists-1 = 0 */
    /* MULTICORE_RENDERING_TILE_LIST_SET_BASE (0x7B) -> binning pool */
    off = cl_u8(off, 0x7B);
    off = cl_u32le(off, _pool_p);
    /* initial TLB clear (2x, global, GFXH-1742 workaround): only on the first
     * dispatch of a frame, which initialises colour (RT_PART1) + Z (ZS_CLEAR)
     * for the whole frame.  Later dispatches LOAD the persisted buffers, so
     * clearing here would erase the frame accumulated so far. */
    if (first_frame) {
        for (i = 0; i < 2; i++) {
            off = cl_u8(off, 0x7C);             /* TILE_COORDINATES (0,0) */
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x1A);             /* END_OF_LOADS */
            off = cl_u8(off, 0x1D);             /* STORE dummy (NONE) */
            off = cl_u8(off, 0x08);
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x00);
            off = cl_u16le(off, 0x00);
            off = cl_u32le(off, 0x00);
            off = cl_u8(off, 0x19);             /* CLEAR_RENDER_TARGETS */
            off = cl_u8(off, 0x1B);             /* END_OF_TILE_MARKER */
        }
    }
    off = cl_u8(off, 0x13);                     /* FLUSH_VCD_CACHE */
    /* generic tile list built directly into _gen (the cl_* helpers write
     * into _cl and would clobber the RCL being assembled there). */
    {
        uint8_t *gp = _gen_va;
        uint32_t g = 0;
        int zpersist = (depth_en && z_iova != 0);
        gp[g++] = 0x7D;                         /* TILE_COORDINATES_IMPLICIT */
        if (!first_frame) {
            /* later dispatch of the frame: reload the persisted colour (+Z)
             * from DRAM so this dispatch accumulates onto the previous ones
             * instead of erasing them. */
            gen_emit_store_load(gp, &g, 0x1E, 0, fmt, rb_swap,
                                stride_bytes, fb_iova);          /* LOAD RT0 */
            if (zpersist)
                gen_emit_store_load(gp, &g, 0x1E, 9, V3D_3D_FMT_D32F, 0,
                                    z_stride, z_iova);           /* LOAD Z */
        }
        gp[g++] = 0x1A;                         /* END_OF_LOADS */
        gp[g++] = 0x38;                         /* PRIM_LIST_FORMAT */
        gp[g++] = prim_list_type(mode);         /* points/lines/triangles */
        gp[g++] = 0x36;                         /* SET_INSTANCEID (PTB expects 0) */
        gp[g++] = 0x00;
        gp[g++] = 0x15;                         /* BRANCH_TO_IMPLICIT_TILE_LIST */
        gp[g++] = 0x00;                         /* set 0 */
        gen_emit_store_load(gp, &g, 0x1D, 0, fmt, rb_swap,
                            stride_bytes, fb_iova);              /* STORE RT0 */
        if (zpersist)
            gen_emit_store_load(gp, &g, 0x1D, 9, V3D_3D_FMT_D32F, 0,
                                z_stride, z_iova);               /* STORE Z */
        gp[g++] = 0x1B;                         /* END_OF_TILE_MARKER */
        gp[g++] = 0x12;                         /* RETURN_FROM_SUB_LIST */
        off = cl_u8(off, 0x14);                 /* START_ADDRESS_OF_GENERIC_TILE_LIST */
        off = cl_u32le(off, _gen_p);
        off = cl_u32le(off, _gen_p + g);
    }
    off = cl_u8(off, 0x17);                     /* SUPERTILE_COORDINATES (0,0) */
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x0D);                     /* END_OF_RENDERING */
    return off;
}

/* ------------------------------------------------------------------ */
/* public entry points                                                   */
/* ------------------------------------------------------------------ */

int v3d_3d_clear(uint32_t fb_iova, uint32_t stride_bytes, uint32_t fmt,
                 uint32_t w, uint32_t h, uint32_t rgba8, int rb_swap)
{
    volatile uint32_t *core;
    uint32_t len;
    int rc, ret = V3D_3D_FAILED;

    if (!_ready)
        return V3D_3D_UNSUPPORTED;
    if (w == 0 || h == 0 || fb_iova == 0)
        return V3D_3D_UNSUPPORTED;
    if (!v3d_g2d_phy_valid(fb_iova, (size_t)stride_bytes * h))
        return V3D_3D_UNSUPPORTED;

    core = v3d_g2d_core_regs();
    v3d_g2d_op_begin();

    /* bin: empty frame -> per-tile lists */
    len = build_bcl_empty(w, h);
    if (len > V3D_3D_CL_MAX)
        goto out;
    rc = submit_bin(core, _cl_p, _cl_p + len);
    if (rc != 0)
        goto out;

    /* render: clear via the explicit per-tile store */
    len = build_clear_rcl(fb_iova, stride_bytes, w, h, 16, fmt, rgba8, rb_swap);
    if (len > V3D_3D_CL_MAX)
        goto out;
    rc = submit_render(core, _cl_p, _cl_p + len);
    if (rc != 0)
        goto out;

    v3d_g2d_l2_flush_all();     /* GPU writes -> DRAM for the display path */
    ret = V3D_3D_DONE;
out:
    v3d_g2d_op_end();
    return ret;
}

int v3d_3d_draw(uint32_t fb_iova, uint32_t stride_bytes, uint32_t fmt,
                uint32_t w, uint32_t h, int mode, uint32_t nverts,
                uint32_t pos_iova, uint32_t col_iova,
                uint32_t rgba, int rb_swap,
                uint32_t depth_func, uint32_t depth_mask, uint32_t cull_en,
                uint32_t z_iova, uint32_t z_stride, int first_frame,
                uint32_t clear_rgba, float clear_z,
                const v3d_3d_ext_t *ext,
                const v3d_3d_tex_t *tex)
{
    volatile uint32_t *core;
    uint32_t len;
    float fx, fy;
    uint32_t ux, uy;
    int rc, ret = V3D_3D_FAILED;
    int tex_path = (tex != NULL && tex->tex_iova != 0 && tex->uv_iova != 0 &&
                    tex->tex_w != 0 && tex->tex_h != 0);

    if (!_ready)
        return V3D_3D_UNSUPPORTED;
    if (w == 0 || h == 0 || fb_iova == 0 || nverts == 0)
        return V3D_3D_UNSUPPORTED;
    if (nverts > V3D_3D_MAX_VERTS)
        return V3D_3D_UNSUPPORTED;
    if (mode != V3D_3D_MODE_POINTS && mode != V3D_3D_MODE_LINES &&
        mode != V3D_3D_MODE_TRIANGLES)
        return V3D_3D_UNSUPPORTED;
    if (!v3d_g2d_phy_valid(fb_iova, (size_t)stride_bytes * h))
        return V3D_3D_UNSUPPORTED;
    if (!v3d_g2d_phy_valid(pos_iova, (size_t)nverts * 12u))
        return V3D_3D_UNSUPPORTED;
    /* col_iova is not read on the tex path (attr slot 1 carries uv instead);
     * uv float3 shares the pos encoding's 12-byte stride. */
    if (tex_path) {
        if (!v3d_g2d_phy_valid(tex->uv_iova, (size_t)nverts * 12u))
            return V3D_3D_UNSUPPORTED;
        if (tex->tex_stride < tex->tex_w * 4u)
            return V3D_3D_UNSUPPORTED;
        if (!v3d_g2d_phy_valid(tex->tex_iova,
                               (size_t)tex->tex_stride * tex->tex_h))
            return V3D_3D_UNSUPPORTED;
    } else if (!v3d_g2d_phy_valid(col_iova, (size_t)nverts * 4u)) {
        return V3D_3D_UNSUPPORTED;
    }
    /* a persistent Z surface is optional (z_iova == 0 -> tile-local Z only);
     * when present it must be a valid Z32F linear surface covering the frame */
    if (z_iova != 0 && !v3d_g2d_phy_valid(z_iova, (size_t)z_stride * h))
        return V3D_3D_UNSUPPORTED;

    /* viewport uniforms: [0]=1.0 w, [1]=x scale (w/2), [2]=y scale (-h/2),
     * [3]=0.5 z scale, [4]=0.5 z off.  Coord shader uniforms: [0]=1.0 w,
     * [1]=x scale, [2]=y scale. */
    fx = (float)(w / 2u);
    fy = -(float)(h / 2u);
    __builtin_memcpy(&ux, &fx, 4);
    __builtin_memcpy(&uy, &fy, 4);
    _unif_va[0] = 0x3F800000u;                  /* 1.0 (w) */
    _unif_va[1] = ux;
    _unif_va[2] = uy;
    _unif_va[3] = 0x3F000000u;                  /* 0.5 z scale */
    _unif_va[4] = 0x3F000000u;                  /* 0.5 z offset */
    _cs_unif_va[0] = 0x3F800000u;               /* coord: 1.0 (w) */
    _cs_unif_va[1] = ux;                        /* coord: vp x scale */
    _cs_unif_va[2] = uy;                        /* coord: vp y scale */

    /* tex fragment uniforms (see _frag_shader_tex's ldunifrf sequence):
     * [0]=tex_base IOVA, [1]=tex_stride (int), [2]=(W-1) float,
     * [3]=(H-1) float, [4]=1/255.0f, [5]=0xFF byte mask, [6]=(W-1) int,
     * [7]=(H-1) int.  Only refreshed on the tex path so a colour draw's
     * stale contents are harmless. */
    if (tex_path) {
        float fw = (float)(tex->tex_w - 1u);
        float fh = (float)(tex->tex_h - 1u);
        uint32_t uw, uh;
        __builtin_memcpy(&uw, &fw, 4);
        __builtin_memcpy(&uh, &fh, 4);
        _tex_unif_va[0] = tex->tex_iova;
        _tex_unif_va[1] = tex->tex_stride;
        _tex_unif_va[2] = uw;                   /* (W-1) float */
        _tex_unif_va[3] = uh;                   /* (H-1) float */
        _tex_unif_va[4] = 0x3B808081u;          /* 1.0f / 255.0f */
        _tex_unif_va[5] = 0x000000FFu;          /* byte mask */
        _tex_unif_va[6] = tex->tex_w - 1u;      /* (W-1) int clamp */
        _tex_unif_va[7] = tex->tex_h - 1u;      /* (H-1) int clamp */
    }

    /* shader record + contiguous attribute records, then the CLs.  The tex
     * path swaps in the point-sampled frag/vtx pair (2 varyings u,v) and the
     * uv float3 attr-1 record; the coord shader and its uniforms are shared
     * because the binner only reads pos. */
    if (tex_path) {
        build_shader_record(_frag_tex_iova, _tex_unif_p,
                            _vtx_tex_iova, _unif_p,
                            _coord_iova, _cs_unif_p, 2u);
        build_attr_records_tex(pos_iova, tex->uv_iova);
    } else {
        build_shader_record(_frag_iova, _unif_p, _vtx_iova, _unif_p,
                            _coord_iova, _cs_unif_p, 4u);
        build_attr_records(pos_iova, col_iova);
    }
    __asm__ __volatile__("dsb sy" ::: "memory");

    core = v3d_g2d_core_regs();
    v3d_g2d_op_begin();

    /* bin: the primitive list */
    len = build_draw_bcl(mode, nverts, _ssr_p, 2, w, h,
                         depth_func, depth_mask, cull_en, ext);
    if (len > V3D_3D_CL_MAX)
        goto out;
    rc = submit_bin(core, _cl_p, _cl_p + len);
    if (rc != 0)
        goto out;

    /* render: implicit per-tile branch to the binned primitives.  The first
     * dispatch of a frame clears colour+Z; later dispatches LOAD the persisted
     * colour+Z so depth and framebuffer accumulate across the frame.  The
     * tile-local Z buffer is enabled iff a depth test was requested. */
    len = build_draw_rcl(fb_iova, stride_bytes, fmt, clear_rgba, rb_swap, w, h,
                         depth_func != V3D_3D_DEPTH_NONE,
                         z_iova, z_stride, first_frame, clear_z, mode);
    if (len > V3D_3D_CL_MAX)
        goto out;
    rc = submit_render(core, _cl_p, _cl_p + len);
    if (rc != 0)
        goto out;

    v3d_g2d_l2_flush_all();
    ret = V3D_3D_DONE;
    (void)rgba;   /* the fixed-function frag shader emits interpolated color;
                   * a solid-rgba variant lands with the Phase 4 shader set. */
out:
    v3d_g2d_op_end();
    return ret;
}
