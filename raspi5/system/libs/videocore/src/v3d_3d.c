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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/dma.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <videocore/vc_g2d.h>
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
#define CLE_CT1CS    0x104u   /* control thread 1 (render) status            */
#define CLE_BFC      0x134u   /* binning flush count                         */
#define CLE_RFC      0x138u   /* render flush count                          */
#define PTB_BPOS     0x30cu   /* binning pool overspill size (kernel zeroes) */
#define PTB_BPCA     0x300u   /* binning pool current address (write cursor) */
#define PTB_BPCS     0x304u   /* binning pool current size                   */
#define CLE_CT0CS    0x100u   /* control thread 0 (binning) status           */
#define CLE_CT0CA    0x110u   /* control thread 0 current CL address         */
#define CLE_CT0RA    0x118u   /* control thread 0 read address               */
#define CLE_CT0LC    0x120u   /* control thread 0 list counter               */
#define CLE_CT0PC    0x128u   /* control thread 0 prim counter               */
#define V3D_ERR_STAT 0xf20u   /* core error status                           */
#define V3D_INT_STS  0x50u    /* interrupt status (flush-complete/error bits)*/

/* ---- staging sizes ---------------------------------------------------- */
#define V3D_3D_CL_MAX      (64u * 1024u)   /* control-list buffer           */
#define V3D_3D_POOL_SIZE   (256u * 1024u)  /* binning tile-alloc pool       */
#define V3D_3D_TSDA_SIZE   (64u * 1024u)   /* tile-state data array         */
#define V3D_3D_SSR_SIZE    64u             /* 32 shader-rec + 2*16 attr-rec */
#define V3D_3D_SHADER_WORDS 160u           /* staged frag+vtx+coord + tex frag/vtx QPU (127 used) */
#define V3D_3D_UNIF_WORDS  8u              /* vtx/frag uniforms             */
#define V3D_3D_GEN_SIZE    256u            /* generic tile list             */
#define V3D_3D_COMPUTE_BUDGET_MS 2000u     /* compiled compute draw wait    */

/* Wall-clock bound for a single bin or render flush wait (ms).  The CSD
 * path uses ~256 ms; a full-frame 3D render is given more head-room, but
 * still finite so a wedged pipeline degrades instead of hanging g2dd. */
#define V3D_3D_WAIT_MS     1000u

/* INTERIM GATE for the compiled-shader 3D draw (binning+render geometry).
 * Device evidence on BCM2712/V3D 7.1 shows the binner geometry path never
 * completes a flush for real primitives: the bin control-thread consumes the
 * whole CL (CT0CA == CL end), runs the coord shader on every vertex
 * (CT0PC == nverts) and writes tile lists to the pool (BPCA advances ~16 KB
 * for 80 tiles), yet CLE_BFC never increments and the wait times out.  No
 * fresh ERR_STAT, no MMU fault -- the flush simply never drains.  The
 * bare-metal reference never solved primitive binning on VC7 either, so this
 * path has NO proven-working precedent on this silicon.  A timed-out job
 * leaves the CLE live and wedges the SHARED V3D block, corrupting the desktop
 * compositor until a power cycle (there is no software V3D reset).  Until a
 * proven execution path exists (CSD compute dispatch of memory-storing
 * shaders), decline compiled draws fast so the client falls back to CPU and
 * the desktop stays clean.  Flip to 1 to re-enable binner bring-up.
 *
 * RE-ENABLED (1) for on-device binner bring-up: the mutex+park dispatch fix
 * means a timed-out bin no longer leaves a g2dd worker spinning at 96%, so the
 * cost of a stall is now a wedged V3D (power cycle) rather than a hung daemon.
 * This lets teapot's real 600-vert geometry bin run so the four BIN-timeout
 * diagnostics can be captured FRESH (all prior captures were either the old
 * As=2 record or a byte-identical stale binary). */
#define V3D_3D_ENABLE_COMPILED_DRAW 1

/* Fixed-function (hand-written _coord/_vtx/_frag shader) draws.  These
 * shaders have never completed a bin on this silicon: the on-device capture
 * shows the coord batch stalling at the first 16 vertices with a QPU
 * interrupt (BIN timeout, ct0pc 0xf, intsts 0x10000) while mesa-compiled
 * programs bin cleanly through the same CL builder.  Every portablegl app
 * reaches this path through pgl_gpu_tri(), so one such draw used to wedge the
 * shared V3D for shadertoy and the desktop too.  Decline them (client
 * rasterizes on the CPU) until the hand-written shaders are proven. */
#define V3D_3D_ENABLE_FIXED_DRAW 0

/* ---- staged QPU code (V3D 7.1 encodings, verbatim from v3d.c) ----------
 * The QPU does not fetch code from .rodata reliably on this silicon, so the
 * three shaders are copied into dma staging at init and referenced by IOVA.
 *
 * Every shader ends "thrsw ; nop ; nop": the thread-end THRSW has two delay
 * slots that still execute (mesa qpu_schedule.c pads the thrend with
 * 3 - slots_filled nops; py-videocore7 ends thrsw/nop/nop).  Without the
 * padding the delay slots run the first two words of whatever is staged next
 * in _shaders (an ldvary / ldvpmv_in of the following shader) while the
 * thread is ending.
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
    0x38000000bb03f000ull, /* nop            ; nop   (thrend delay slot 1)  */
    0x38000000bb03f000ull, /* nop            ; nop   (thrend delay slot 2)  */
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
    0x38000000bb03f000ull, /* nop                    ; nop (thrend delay slot 1) */
    0x38000000bb03f000ull, /* nop                    ; nop (thrend delay slot 2) */
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
    0x38000000bb03f000ull, /* nop                    ; nop (thrend delay slot 1) */
    0x38000000bb03f000ull, /* nop                    ; nop (thrend delay slot 2) */
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
    0x38000000bb03f000ull, /* nop            ; nop   (thrend delay slot 1)    */
    0x38000000bb03f000ull, /* nop            ; nop   (thrend delay slot 2)    */
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
    0x38000000bb03f000ull, /* nop                    ; nop (thrend delay slot 1) */
    0x38000000bb03f000ull, /* nop                    ; nop (thrend delay slot 2) */
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
static uint32_t *_cvs_unif_va; static uint32_t _cvs_unif_p;/* compiled VS expanded uniforms */
static uint32_t *_cfs_unif_va; static uint32_t _cfs_unif_p;/* compiled FS expanded uniforms */
static uint32_t *_ccs_unif_va; static uint32_t _ccs_unif_p;/* compiled COORD expanded uniforms */
static uint32_t *_ccu_unif_va; static uint32_t _ccu_unif_p;/* compiled COMPUTE expanded uniforms */
/* GPU-visible default-uniform storage for indirect ldunifa/TMU loads.  The
 * client's blob is capped at PGL_G3D_MAX_UNIF (256 dwords). */
#define V3D_3D_JOB_MAX_UNIF 256u
static uint32_t *_job_unif_va; static uint32_t _job_unif_p;
static uint8_t  *_gen_va;     static uint32_t _gen_p;   /* generic tile list */
/* Two-pass geometry rasterizer intermediate: `vout` holds one packed record per
 * vertex, written by the pass-1 vertex-transform kernel and read by the pass-2
 * raster kernel.  The gouraud / textured / instanced variants pack 8 floats
 * (32 bytes); the colour-modulated textured variant (cs_is_geom == 4, imgui)
 * also carries a per-vertex colour, so its record is 12 floats (48 bytes).  The
 * buffer is sized for the widest record so every variant fits.  Sized for
 * V3D_3D_MAX_VERTS vertices; a single process-lifetime buffer is safe because
 * CSD dispatches are serialized (v3d_g2d_op_begin) and each geom draw fully
 * overwrites the sub-range it consumes. */
static uint32_t *_vout_va;    static uint32_t _vout_p;
#define V3D_3D_VOUT_BYTES (V3D_3D_MAX_VERTS * 48u)

/* WS4 compiled-shader sampler bindings: per-unit V3D71 TEXTURE_SHADER_STATE and
 * SAMPLER_STATE records.  A mesa-compiled FS that samples iChannelN emits a
 * QUNIFORM_TMU_CONFIG_P0/P1 pair per unit whose expansion ORs a 16-byte-aligned
 * state-record IOVA into the template's low bits, so each record lives in its
 * own 32-byte slot (V3D_3D_TEXSTATE_SLOT) to keep bits [3:0] free.  The sampler
 * records are identical for every shadertoy texture (GL_NEAREST + GL_REPEAT) and
 * are built once at init; the texture records are rebuilt each draw from ctex. */
#define V3D_3D_TEXSTATE_SLOT 32u
static uint8_t  *_texstate_va; static uint32_t _texstate_p; /* 4 texture records */
static uint8_t  *_sampler_va;  static uint32_t _sampler_p;  /* 4 sampler records */

/* staged shader addresses inside _shaders (IOVA), computed at init */
static uint32_t _frag_iova, _vtx_iova, _coord_iova;
static uint32_t _frag_tex_iova, _vtx_tex_iova;

static int _ready;

/* ---- compiled-program cache (WS4) ---------------------------------- */
/* Each uploaded program's GPU-resident IOVAs (VS/FS QPU + static uniform
 * value streams) plus its CPU-side prog_data (uniform layout descriptors,
 * VPM sizes, attribute/input maps, FS flags) are cached here by handle.  A
 * later draw references the handle to build the shader record + per-draw
 * uniform buffer.  Sized for the shadertoy effects plus head-room; the inline
 * descriptor arrays bound the uniform stream (an out-of-range program is
 * declined by v3d_3d_program, never truncated). */
typedef struct {
    int      used;
    /* GPU-resident QPU code (dma IOVA + word count) */
    uint32_t vs_qpu_iova;   uint32_t vs_qpu_words;
    uint32_t fs_qpu_iova;   uint32_t fs_qpu_words;
    uint32_t coord_qpu_iova; uint32_t coord_qpu_words; /* binning coord shader */
    /* raw dma_alloc VAs of the four QPU code blocks (0 when absent) so
     * v3d_3d_program_free can hand them back; the IOVAs above point into the
     * page-rounded interior of these blocks and cannot be freed directly. */
    ewokos_addr_t vs_qpu_raw, fs_qpu_raw, coord_qpu_raw, cs_qpu_raw, cs_spill_raw;
    /* GPU-resident static uniform value streams (dma IOVA); 0 when the stage
     * has no uniforms.  A per-draw buffer is patched from these at draw. */
    uint32_t vs_unif_iova;  uint32_t vs_num_uniforms;
    uint32_t fs_unif_iova;  uint32_t fs_num_uniforms;
    uint32_t coord_unif_iova; uint32_t coord_num_uniforms;
    /* CPU-side uniform layout descriptors (how to build per-draw uniforms) */
    uint32_t vs_unif_contents[V3D_3D_PROG_MAX_UNIF];
    uint32_t fs_unif_contents[V3D_3D_PROG_MAX_UNIF];
    uint32_t coord_unif_contents[V3D_3D_PROG_MAX_UNIF];
    /* CPU-side per-slot uniform ARGS (the mesa `uniforms[]` stream).  The
     * QUNIFORM interpreter reads these + the per-draw GL uniform blob at draw
     * time and expands each slot into ONE u32 in the shared per-draw dma
     * staging (_cvs_unif / _cfs_unif); the raw arg stream is never handed to
     * the GPU directly.  vs_unif_iova / fs_unif_iova above are therefore only
     * a 0 sentinel on the compiled path (no per-program dma upload). */
    uint32_t vs_unif_data[V3D_3D_PROG_MAX_UNIF];
    uint32_t fs_unif_data[V3D_3D_PROG_MAX_UNIF];
    uint32_t coord_unif_data[V3D_3D_PROG_MAX_UNIF];
    /* vertex-stage prog_data for the shader record */
    uint8_t  vs_threads, vs_single_seg;
    uint32_t vs_vpm_input_size, vs_vpm_output_size, vs_vcm_cache_size;
    uint8_t  vs_separate_segments;
    uint8_t  vs_vattr_sizes[V3D_3D_PROG_MAX_ATTRS / 4];
    int32_t  vs_driver_location_map[V3D_3D_PROG_MAX_ATTRS];
    /* fragment-stage prog_data for the shader record */
    uint8_t  fs_threads, fs_single_seg;
    uint8_t  fs_needs_coords, fs_uses_center_w, fs_writes_z, fs_disable_ez;
    uint32_t fs_num_inputs;
    uint32_t fs_input_slots[V3D_3D_PROG_MAX_INPUTS];
    /* coordinate-stage prog_data: the shader record's coord VPM-segment fields.
     * The coord shader writes only gl_Position (no varyings), so its VPM output
     * size differs from the render VS's and must be programmed separately. */
    uint8_t  coord_threads, coord_single_seg;
    uint32_t coord_vpm_input_size, coord_vpm_output_size, coord_vcm_cache_size;
    uint8_t  coord_separate_segments;
    /* optional compute stage (shadertoy compute route); cs_qpu_words == 0 when
     * absent.  Dispatched through v3d_g2d_run_csd instead of the binner. */
    uint32_t cs_qpu_iova;   uint32_t cs_qpu_words;
    uint32_t cs_num_uniforms;
    uint32_t cs_unif_contents[V3D_3D_PROG_MAX_UNIF];
    uint32_t cs_unif_data[V3D_3D_PROG_MAX_UNIF];
    uint8_t  cs_threads, cs_single_seg;
    uint32_t cs_local_size[3];
    uint32_t cs_shared_size;
    uint32_t cs_spill_size;   /* bytes/thread of register-spill scratch (0 = none) */
    uint32_t cs_spill_iova;   /* scratch DMA base IOVA: the QUNIFORM_SPILL_OFFSET value */
    uint32_t cs_is_geom;    /* 1 = gouraud geom, 2 = textured, 3 = instanced,
                             * 4 = colour-modulated textured (imgui) */
    uint32_t cs_geom_tpw;   /* raster triangles/workgroup: 0 = one wg per tri */
    /* second compute stage (cs2 = pass-1 vertex transform of the two-pass
     * geometry rasterizer).  cs2_qpu_words == 0 when absent.  Dispatched FIRST
     * (one invocation per vertex) writing the packed `vout` records the cs
     * raster pass then reads.  cs2_uniform_blob_dwords splits the per-draw
     * uniform blob: [0, cs2_uniform_blob_dwords) feeds cs2, the rest feeds cs. */
    uint32_t cs2_qpu_iova;  uint32_t cs2_qpu_words;
    ewokos_addr_t cs2_qpu_raw;
    uint32_t cs2_num_uniforms;
    uint32_t cs2_unif_contents[V3D_3D_PROG_MAX_UNIF];
    uint32_t cs2_unif_data[V3D_3D_PROG_MAX_UNIF];
    uint8_t  cs2_threads, cs2_single_seg;
    uint32_t cs2_local_size[3];
    uint32_t cs2_shared_size;
    uint32_t cs2_uniform_blob_dwords;
} v3d_3d_prog_t;

static v3d_3d_prog_t _progs[V3D_3D_MAX_PROGS];

/* ------------------------------------------------------------------ */
/* dma staging allocation: dma_alloc is not page-aligned, so over-       */
/* allocate and round the VA up to a page (the contiguous dma mapping    */
/* keeps phys page-aligned too).  Mirrors v3d_g2d.c's MMU-table alloc.   */
/* ------------------------------------------------------------------ */
static int alloc_stage_raw(uint32_t size, void **va, uint32_t *iova,
                           ewokos_addr_t *raw)
{
    ewokos_addr_t r = dma_alloc(0, size + 4095u);
    ewokos_addr_t v, p;

    if (r == 0)
        return -1;
    v = (r + 4095u) & ~(ewokos_addr_t)4095u;
    p = dma_phy_addr(0, v);
    /* the buffer must be inside a RAM range present in the V3D page table */
    if (p == 0 || (p >> 32) != 0 || !v3d_g2d_phy_valid(p, size)) {
        dma_free(0, r);
        return -1;
    }
    *va = (void *)(uintptr_t)v;
    *iova = (uint32_t)p;
    if (raw != NULL)
        *raw = r;
    return 0;
}

/* permanent staging (never freed): the raw block handle is not needed */
static int alloc_stage(uint32_t size, void **va, uint32_t *iova)
{
    return alloc_stage_raw(size, va, iova, NULL);
}

/* Publish a just-memcpy'd dma block to the GPU.  The ARM writes dma through a
 * NOCACHE mapping, so DRAM is already up to date once the store buffer drains;
 * what is NOT up to date is the V3D L2T, which can still hold clean lines the
 * previous owner of this recycled IOVA fetched (see v3d_g2d_invalidate_span).
 * Skipping this made a freshly uploaded CL kernel silently execute the program
 * that previously lived at the same address: the dispatch completed with no
 * fault and no MMU error, but the new kernel's stores never happened. */
static void publish_stage(uint32_t iova, uint32_t bytes)
{
    __asm__ __volatile__("dsb sy" ::: "memory");
    v3d_g2d_invalidate_span(iova, bytes);
}

/* hand a program's QPU code blocks back to dma and clear the slot */
static void release_prog(v3d_3d_prog_t *pr)
{
    if (pr->vs_qpu_raw)    dma_free(0, pr->vs_qpu_raw);
    if (pr->fs_qpu_raw)    dma_free(0, pr->fs_qpu_raw);
    if (pr->coord_qpu_raw) dma_free(0, pr->coord_qpu_raw);
    if (pr->cs_qpu_raw)    dma_free(0, pr->cs_qpu_raw);
    if (pr->cs_spill_raw)  dma_free(0, pr->cs_spill_raw);
    if (pr->cs2_qpu_raw)   dma_free(0, pr->cs2_qpu_raw);
    memset(pr, 0, sizeof(*pr));
}

/* ------------------------------------------------------------------ */
/* WS4 compiled-shader sampler state records.                            */
/*                                                                       */
/* A mesa-compiled FS that samples iChannelN emits, per texture unit, a    */
/* QUNIFORM_TMU_CONFIG_P0/P1 pair whose data word is (unit << 24) | low:   */
/* P0 low bits [3:0] are return_words_of_texture_data and P1 low bits      */
/* [2:0] are the per_pixel_mask/unnormalized/output_type_32 flags (see     */
/* broadcom/compiler/v3d_tex.c ntq_emit_tmu_general).  The gallium driver  */
/* expands each into a hardware address by OR-ing those low bits into a    */
/* TEXTURE_SHADER_STATE (P0) / SAMPLER_STATE (P1) record's IOVA, so the    */
/* records must be 16-byte aligned (each lives in its own 32-byte slot).   */
/* gallium's v3d_texture.c is NOT vendored here, so the record contents    */
/* are transcribed field-by-field from the authoritative encodings in the  */
/* vendored tree: the bit layout from cle/v3d_packet_v71_pack.h, the RGBA8  */
/* texture_type from enum V3D71_Texture_Data_Formats (=4), the swizzle     */
/* values from common/v3d_util.c v3d_translate_pipe_swizzle (RED=2 ..      */
/* ALPHA=5), and the 64-byte pixel-data alignment from the packet XML's    */
/* address-field note.  Textures are single-mip linear RGBA8, GL_REPEAT,   */
/* with the mag/min filter chosen per unit by the client (ctex_filter).   */
/* ------------------------------------------------------------------ */

/* Set the little-endian bit field [lo..hi] (inclusive; bit 0 == rec[0] bit
 * 0) of a packed 24-byte record to val, mirroring mesa's util_bitpack_uint
 * so the layout matches the V3D71_*_pack functions exactly. */
static void rec_set_bits(uint8_t *rec, uint32_t lo, uint32_t hi, uint32_t val)
{
    uint32_t b;
    for (b = lo; b <= hi; b++) {
        uint8_t mask = (uint8_t)(1u << (b & 7u));
        if ((val >> (b - lo)) & 1u)
            rec[b >> 3] |= mask;
        else
            rec[b >> 3] &= (uint8_t)~mask;
    }
}

/* Pack a single-level, non-array RGBA8 V3D71_TEXTURE_SHADER_STATE (24 bytes)
 * sampling the w x h image at base_iova (which must be 64-byte aligned: bits
 * 0-5 of texture_base_pointer are overlaid by the reverse / transfer_func /
 * flip flags, all 0 here).  tiling is the V3D_3D_TEX_TILING_* layout the
 * client stored the pixels in (the TMU has no raster 2D mode): for the LT /
 * UBLINEAR layouts the hardware infers the layout from the image size exactly
 * as gallium's v3d_setup_slices chose it, so the level-0 fields stay 0; a
 * UIF level 0 is declared explicitly (level_0_is_strictly_uif / xor_enable /
 * ub_pad) and that requires the extended record (gallium v3dx_state.c
 * create_texture_shader_state_bo sets extended whenever strictly_uif or
 * uif_xor_disable is set). */
static void pack_tex_state_rgba8(uint8_t *rec, uint32_t base_iova,
                                 uint32_t w, uint32_t h,
                                 uint32_t tiling, uint32_t ub_pad,
                                 uint32_t depth, uint32_t arrstride64)
{
    /* image_width/image_height are the TMU's uv->texel scale.  An on-device
     * texel probe (macgl `probe` mode over a 4x512 LINEARTILE atlas) measured
     * the effective scale as exactly 3 x 511 = (w-1) x (h-1) with a 100% fit
     * over 5328 samples, i.e. this silicon uses the stored field DIRECTLY as
     * the multiplier and does NOT add 1 the way the gallium "field = dim - 1"
     * convention assumes.  Storing w-1/h-1 therefore mis-scaled every texture2D
     * fetch in a compiled compute draw (col >= 2 and row >= h/2 landed on the
     * neighbouring texel), corrupting lookup-table reads.  Store the true
     * dimensions so floor(uv * dim) resolves the requested texel. */
    uint32_t width  = w;
    uint32_t height = h;
    uint32_t uif = (tiling == V3D_3D_TEX_TILING_UIF_NO_XOR ||
                    tiling == V3D_3D_TEX_TILING_UIF_XOR);

    memset(rec, 0, 24);
    rec_set_bits(rec,   0,  31, base_iova);   /* texture_base_pointer      */
    /* reverse(5) / transfer_func(2-4) / flip_y(1) / flip_x(0) = 0;
     * r_b_swap(32) = 0.  array_stride_64_byte_aligned(33-56) is the per-layer
     * pitch of a 2D array in 64-byte units; 0 for a plain single-layer 2D
     * image, layer_size/64 for a cubemap staged as a 6-layer array (mesa
     * lowers a samplerCube TMU fetch to a 2D-array + face-index sample). */
    rec_set_bits(rec,  33,  56, arrstride64); /* array_stride_64_byte_aligned */
    rec_set_bits(rec,  58,  71, width);       /* image_width  = w (TMU scale) */
    rec_set_bits(rec,  72,  85, height);      /* image_height = h (TMU scale) */
    rec_set_bits(rec,  86,  99, depth ? depth - 1u : 0u); /* image_depth */
    rec_set_bits(rec, 100, 106, 4u);          /* texture_type = RGBA8      */
    rec_set_bits(rec, 107, 107, uif);         /* extended (level-0 fields) */
    rec_set_bits(rec, 108, 110, 2u);          /* swizzle_r = RED           */
    rec_set_bits(rec, 111, 113, 3u);          /* swizzle_g = GREEN         */
    rec_set_bits(rec, 114, 116, 4u);          /* swizzle_b = BLUE          */
    rec_set_bits(rec, 117, 119, 5u);          /* swizzle_a = ALPHA         */
    rec_set_bits(rec, 120, 123, 0u);          /* max_level  = 0            */
    rec_set_bits(rec, 124, 127, 0u);          /* base_level = 0            */
    if (uif) {
        rec_set_bits(rec, 128, 131, ub_pad & 0xFu);  /* level_0_ub_pad     */
        rec_set_bits(rec, 132, 132,                  /* level_0_xor_enable */
                     tiling == V3D_3D_TEX_TILING_UIF_XOR ? 1u : 0u);
        rec_set_bits(rec, 134, 134, 1u);             /* level_0_is_strictly_uif */
    }
    rec_set_bits(rec, 135, 135, 0u);          /* uif_xor_disable = 0       */
    /* chroma_offset_x/y, texture_base_pointer_cb/cr, pad = 0 (RGB, not YUV). */
}

/* Pack a GL_REPEAT V3D71_SAMPLER_STATE (24 bytes) with the given mag/min
 * filter (V3D_3D_TEX_FILTER_*): NEAREST sets the *_filter_nearest bits,
 * LINEAR clears them so the TMU bilinearly blends the 2x2 footprint in
 * hardware (one fetch instead of the four a shader does by hand).  mip filter
 * stays nearest (single-level images, LOD clamped to 0), sRGB disabled,
 * REPEAT wrap on all axes, no anisotropy, no border/depth-compare. */
static void pack_sampler_state(uint8_t *rec, uint32_t filter)
{
    uint32_t nearest = (filter == V3D_3D_TEX_FILTER_LINEAR) ? 0u : 1u;
    memset(rec, 0, 24);
    rec_set_bits(rec,  0,  0, nearest);   /* mag_filter_nearest    */
    rec_set_bits(rec,  1,  1, nearest);   /* min_filter_nearest    */
    rec_set_bits(rec,  2,  2, 1u);   /* mip_filter_nearest        */
    rec_set_bits(rec,  3,  3, 0u);   /* anisotropy_enable         */
    rec_set_bits(rec,  4,  6, 0u);   /* depth_compare_function    */
    rec_set_bits(rec,  7,  7, 1u);   /* srgb_disable              */
    rec_set_bits(rec,  8, 19, 0u);   /* min_level_of_detail = 0.0 */
    rec_set_bits(rec, 20, 31, 0u);   /* max_level_of_detail = 0.0 */
    rec_set_bits(rec, 32, 47, 0u);   /* fixed_bias = 0.0          */
    rec_set_bits(rec, 48, 50, 0u);   /* wrap_s = REPEAT           */
    rec_set_bits(rec, 51, 53, 0u);   /* wrap_t = REPEAT           */
    rec_set_bits(rec, 54, 56, 0u);   /* wrap_r = REPEAT           */
    rec_set_bits(rec, 57, 57, 0u);   /* wrap_i_border             */
    rec_set_bits(rec, 58, 60, 0u);   /* border_color_mode         */
    rec_set_bits(rec, 61, 62, 0u);   /* maximum_anisotropy        */
    /* border_color_word_0..3 (64-191) = 0. */
}

/* Initial sampler records: NEAREST on every unit (what a draw with no ctex
 * samples through, and the filter a client that never sets one gets). */
static void build_sampler_records(void)
{
    uint32_t u;
    for (u = 0; u < V3D_3D_MAX_TEX_UNITS; u++)
        pack_sampler_state(_sampler_va + u * V3D_3D_TEXSTATE_SLOT,
                           V3D_3D_TEX_FILTER_NEAREST);
}

/* Rebuild the per-unit texture AND sampler records from this draw's ctex
 * bindings.  A unit with ctex_iova == 0 stays unbound (zeroed texture record,
 * NEAREST sampler).  Returns 0 on success, -1 if a bound unit's pixel data is
 * not 64-byte aligned or is still in raster layout (the TMU cannot sample it;
 * the caller then declines to the CPU path rather than sample a scrambled
 * image). */
static int build_tex_state_records(const v3d_3d_ctex_t *ctex)
{
    uint32_t u;
    if (ctex == NULL)
        return 0;
    for (u = 0; u < V3D_3D_MAX_TEX_UNITS; u++) {
        uint8_t *rec = _texstate_va + u * V3D_3D_TEXSTATE_SLOT;
        uint8_t *srec = _sampler_va + u * V3D_3D_TEXSTATE_SLOT;
        if (ctex->ctex_iova[u] == 0) {
            memset(rec, 0, 24);
            pack_sampler_state(srec, V3D_3D_TEX_FILTER_NEAREST);
            continue;
        }
        if ((ctex->ctex_iova[u] & 0x3Fu) != 0)
            return -1;
        if (ctex->ctex_tiling[u] == V3D_3D_TEX_TILING_RASTER ||
            ctex->ctex_tiling[u] > V3D_3D_TEX_TILING_UIF_XOR) {
            slog("v3d_3d: ctex unit %u tiling %u unsupported -> decline\r\n",
                 u, (uint32_t)ctex->ctex_tiling[u]);
            return -1;
        }
        pack_tex_state_rgba8(rec, ctex->ctex_iova[u],
                             ctex->ctex_w[u], ctex->ctex_h[u],
                             ctex->ctex_tiling[u], ctex->ctex_ub_pad[u],
                             ctex->ctex_depth[u], ctex->ctex_arrstride64[u]);
        pack_sampler_state(srec, ctex->ctex_filter[u]);
    }
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

    /* compiled-draw per-stage expanded-uniform staging (WS4): one shared buffer
     * per stage, filled by the QUNIFORM interpreter at draw time and referenced
     * by the shader-state record.  Dispatches are serialized (v3d_g2d_op_begin)
     * so a single shared buffer per stage is safe.  Sized for the largest
     * cached uniform stream (V3D_3D_PROG_MAX_UNIF words). */
    if (alloc_stage(V3D_3D_PROG_MAX_UNIF * 4u, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _cvs_unif_va = (uint32_t *)va; _cvs_unif_p = iova;

    if (alloc_stage(V3D_3D_PROG_MAX_UNIF * 4u, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _cfs_unif_va = (uint32_t *)va; _cfs_unif_p = iova;

    /* compiled coordinate-shader (binning) expanded-uniform staging: the coord
     * stage is the same VS NIR compiled with is_coord=true, so it shares the
     * render VS's uniform driver_locations and is expanded from the SAME
     * per-draw blob into its own buffer (the coord QPU references its own
     * uniform stream, which dead-strips the lighting uniforms but keeps e.g.
     * mvp at the identical dword offset). */
    if (alloc_stage(V3D_3D_PROG_MAX_UNIF * 4u, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _ccs_unif_va = (uint32_t *)va; _ccs_unif_p = iova;

    /* compiled compute-stage expanded-uniform staging: the CSD fetches the
     * expanded stream straight from here (cfg[6]). */
    if (alloc_stage(V3D_3D_PROG_MAX_UNIF * 4u, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _ccu_unif_va = (uint32_t *)va; _ccu_unif_p = iova;

    if (alloc_stage(V3D_3D_JOB_MAX_UNIF * 4u, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _job_unif_va = (uint32_t *)va; _job_unif_p = iova;

    if (alloc_stage(V3D_3D_GEN_SIZE, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _gen_va = (uint8_t *)va; _gen_p = iova;

    /* two-pass geometry rasterizer intermediate (see _vout_va above) */
    if (alloc_stage(V3D_3D_VOUT_BYTES, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _vout_va = (uint32_t *)va; _vout_p = iova;

    /* WS4 compiled-shader sampler state records: 4 texture + 4 sampler records,
     * each in its own 32-byte slot (16-byte aligned so the TMU_CONFIG low bits
     * OR cleanly).  The sampler records are identical for every shadertoy
     * texture, so build them once here; the texture records are rebuilt each
     * draw from ctex (build_tex_state_records). */
    if (alloc_stage(V3D_3D_MAX_TEX_UNITS * V3D_3D_TEXSTATE_SLOT, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _texstate_va = (uint8_t *)va; _texstate_p = iova;

    if (alloc_stage(V3D_3D_MAX_TEX_UNITS * V3D_3D_TEXSTATE_SLOT, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _sampler_va = (uint8_t *)va; _sampler_p = iova;
    build_sampler_records();

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
    slog("v3d_3d: 3D pipeline staging ready (cl %u pool %u tsda %u)\r\n",
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

/* Low 3 bits of each shader code address in the GL Shader State Record
 * (V3D71 GL_SHADER_STATE_RECORD, v3d_packet_v71_pack.h: cl[8]/[16]/[24] =
 * addr | 4_way_threadable<<0 | start_in_final_thread_section<<1 |
 * propagate_nans<<2).  The hardware uses them to decide how the thread is
 * spawned and, critically, when it may END:
 *   start_in_final_thread_section: the QPU is launched already past its
 *     "last THRSW" so the single program-end THRSW terminates the thread.
 *     Mesa sets this from prog_data->single_seg, which is !c->last_thrsw
 *     (vir.c); non-fragment shaders without TMU traffic never get a last
 *     THRSW pair (nir_to_vir.c vir_emit_last_thrsw skips them), so every
 *     coord/vertex shader here has single_seg=1.  With the bit CLEAR the
 *     hardware waits for a THRSW pair that never comes: the coord thread
 *     never ends, its VPM segment is never released, the VCD stalls once the
 *     VPM fills (CT0PC frozen at ~510 verts) and CLE_BFC never increments --
 *     the device-observed BIN timeout with no MMU fault and no ERR_STAT bit.
 *   4_way_threadable: threads == 4 (mesa v3d_pack_shader_state).
 *   propagate_nans: mesa sets it unconditionally. */
#define SH_FLAG_4WAY       (1u << 0)
#define SH_FLAG_FINAL_SEG  (1u << 1)
#define SH_FLAG_PROP_NANS  (1u << 2)

static uint32_t sh_flags(uint32_t threads, uint32_t single_seg)
{
    return (threads == 4u ? SH_FLAG_4WAY : 0u) |
           (single_seg ? SH_FLAG_FINAL_SEG : 0u) |
           SH_FLAG_PROP_NANS;
}

static void build_shader_record(uint32_t frag_addr, uint32_t frag_unif,
                                uint32_t frag_flags,
                                uint32_t vtx_addr, uint32_t vtx_unif,
                                uint32_t vtx_flags,
                                uint32_t coord_addr, uint32_t coord_unif,
                                uint32_t coord_flags,
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
    r[8]  = (uint8_t)((frag_addr & 0xF8) | (frag_flags & 0x07u));
    r[9]  = (uint8_t)((frag_addr >> 8) & 0xFF);
    r[10] = (uint8_t)((frag_addr >> 16) & 0xFF);
    r[11] = (uint8_t)((frag_addr >> 24) & 0xFF);
    r[12] = (uint8_t)(frag_unif & 0xFF);
    r[13] = (uint8_t)((frag_unif >> 8) & 0xFF);
    r[14] = (uint8_t)((frag_unif >> 16) & 0xFF);
    r[15] = (uint8_t)((frag_unif >> 24) & 0xFF);
    r[16] = (uint8_t)((vtx_addr & 0xF8) | (vtx_flags & 0x07u));
    r[17] = (uint8_t)((vtx_addr >> 8) & 0xFF);
    r[18] = (uint8_t)((vtx_addr >> 16) & 0xFF);
    r[19] = (uint8_t)((vtx_addr >> 24) & 0xFF);
    r[20] = (uint8_t)(vtx_unif & 0xFF);
    r[21] = (uint8_t)((vtx_unif >> 8) & 0xFF);
    r[22] = (uint8_t)((vtx_unif >> 16) & 0xFF);
    r[23] = (uint8_t)((vtx_unif >> 24) & 0xFF);
    r[24] = (uint8_t)((coord_addr & 0xF8) | (coord_flags & 0x07u));
    r[25] = (uint8_t)((coord_addr >> 8) & 0xFF);
    r[26] = (uint8_t)((coord_addr >> 16) & 0xFF);
    r[27] = (uint8_t)((coord_addr >> 24) & 0xFF);
    r[28] = (uint8_t)(coord_unif & 0xFF);
    r[29] = (uint8_t)((coord_unif >> 8) & 0xFF);
    r[30] = (uint8_t)((coord_unif >> 16) & 0xFF);
    r[31] = (uint8_t)((coord_unif >> 24) & 0xFF);
}

/* Compiled-shader variant of build_shader_record: lays down the identical
 * flags / varying count / three code+uniform address pairs, then OVERWRITES
 * the four VPM-segment bytes [4..7] with the compiled stages' real sizes from
 * prog_data instead of the fixed pass-through constants (0x01/0x10).  A
 * compiled coordinate shader (is_coord VS) and render VS have different VPM
 * input/output segment sizes than the hand-written fixed shaders, so they must
 * come from the compiler.  Encoding matches mesa's v3d_pack_shader_state via
 * the V3D71 pack header (v3d_packet_v71_pack.h):
 *   byte4 = coord_out_seg | (min_coord_out_in_play_addl_vcm << 4)
 *   byte5 = coord_in_seg  | ((min_coord_in_in_play - 1) << 4)
 *   byte6 = vtx_out_seg   | (min_vtx_out_in_play_addl_vcm << 4)
 *   byte7 = vtx_in_seg    | ((min_vtx_in_in_play - 1) << 4)
 * For a program with no geometry shader v3d_compute_vpm_config fixes
 * As = 1 and Ve = 0, so (As - 1) = 0 and Ve = 0: every "min segments in
 * play" high nibble is 0 and only the four segment sizes (low nibbles) vary.
 * The VCM cache size (Vc) is a VPM-memory-pressure hint mesa programs into a
 * separate VPM config; this pipeline relies on the hardware default (as the
 * fixed path already does), so Vc is cached but not written here. */
static void build_shader_record_compiled(uint32_t frag_addr, uint32_t frag_unif,
                                         uint32_t frag_threads, uint32_t frag_single_seg,
                                         uint32_t vtx_addr, uint32_t vtx_unif,
                                         uint32_t vtx_threads, uint32_t vtx_single_seg,
                                         uint32_t coord_addr, uint32_t coord_unif,
                                         uint32_t coord_threads, uint32_t coord_single_seg,
                                         uint32_t varyings,
                                         uint32_t cs_vpm_out, uint32_t cs_vpm_in,
                                         uint32_t vs_vpm_out, uint32_t vs_vpm_in)
{
    uint8_t *r = _ssr_va;                 /* bytes [0..31] */

    build_shader_record(frag_addr, frag_unif, sh_flags(frag_threads, frag_single_seg),
                        vtx_addr, vtx_unif, sh_flags(vtx_threads, vtx_single_seg),
                        coord_addr, coord_unif, sh_flags(coord_threads, coord_single_seg),
                        varyings);
    /* V3D 7.1 VPM config (proven bare-metal record, v3d71_coord_shader_
     * encoding.md S5 verified on real Pi5 + mesa v3d_packet.xml field layout):
     *   byte5 = coord_in_seg | ((min_coord_in_in_play - 1) << 4)
     *   byte7 = vtx_in_seg   | ((min_vtx_in_in_play   - 1) << 4)
     * The high nibble is "min segments in play" MINUS ONE, so mesa's As=1
     * (v3d_compute_vpm_config, no geometry shader) encodes as 0 -- NOT 1.  The
     * previous 0x10 put 1 in the high nibble => As=2: the binner would not
     * launch the coord shader until TWO input segments were in play.  A 4-vert
     * single-batch quad (shadertoy) flushes before that ever bites, but a
     * pipelined multi-batch mesh (teapot: 600 verts/patch) can never keep two
     * input segments ahead, so the VCM blocks forever -> BIN timeout with
     * ct0cs=CTRUN(0x20), bfc never advancing, the whole CL consumed and NO MMU
     * fault (all device-confirmed).  The low nibble is the INPUT segment size
     * in 8-word sectors and must be >=1 so the coord/vtx shader's ldvpmv_in has
     * VPM to read; mesa folds input into output for its own shared-segment
     * packing (vpm_input_size=0), but the proven hand-rolled record reserves
     * one input sector.  Use the compiler's real input size when it supplies
     * one, else 1.  Only the OUTPUT sizes otherwise vary per compiled program
     * (8-word sectors, clamped >=1). */
    r[4] = (uint8_t)((cs_vpm_out & 0xFu) ? (cs_vpm_out & 0xFu) : 1u);
    r[5] = (uint8_t)((cs_vpm_in  & 0xFu) ? (cs_vpm_in  & 0xFu) : 1u);  /* coord in seg>=1, As=1 */
    r[6] = (uint8_t)((vs_vpm_out & 0xFu) ? (vs_vpm_out & 0xFu) : 1u);
    r[7] = (uint8_t)((vs_vpm_in  & 0xFu) ? (vs_vpm_in  & 0xFu) : 1u);  /* vtx   in seg>=1, As=1 */
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
        /* >200us so libc takes the SYS_USLEEP park branch (0% CPU) instead of
         * the <=200us fine-counter spin: a bin/render flush that never lands
         * (V3D 3D timeout) must not peg a g2dd core for the whole wait.  The
         * deadline above is wall-clock (kernel_tic_ms), so the one-tick park
         * granularity (~976us) does not shorten V3D_3D_WAIT_MS. */
        usleep(500);
    }
    return -1;
}

/* Description of the job currently on the CLE, set by v3d_3d_clear/draw before
 * they ring CT0 and printed with any timeout so the stalling client/program is
 * identifiable from /dev/log alone. */
static char _job_desc[96] = "none";

/* A bin/render flush never landed: the CLE thread is parked mid-job and no
 * later dispatch would complete behind it.  Reset the block (Linux
 * v3d_reset) so the 2D compositor, CSD effects and other 3D clients keep the
 * GPU; v3d_g2d_recover latches the wedge itself when the reset fails or the
 * per-boot cap is reached. */
static void stalled(const char *why)
{
    slog("v3d_3d: %s on job: %s\r\n", why, _job_desc);
    v3d_g2d_recover(why);
}

/* submit the binning CL: point the pool + tile-state at the dma staging,
 * drop stale GPU cache lines, ring CT0, wait for the bin flush count. */
static int submit_bin(volatile uint32_t *core, uint32_t start, uint32_t end)
{
    uint32_t before = core[CLE_BFC / 4];
    /* ERR_STAT is a sticky latch the port never clears.  Writing ~0 to it (the
     * VC4/pi3 reference idiom) is NOT safe on VC7: the VC7 reference only ever
     * READS 0xf20, and adding a ~0 write here made even the empty-clear bin
     * time out (pgl_g3d probe-clear -> "init FAIL probe-clear").  Detect a
     * FRESH error by read-only difference instead -- snapshot before the ring,
     * and after a timeout report only the bits that appeared during this bin. */
    uint32_t err_before = core[V3D_ERR_STAT / 4];
    int rc;

    core[CLE_CT0QMA / 4] = _pool_p;
    core[CLE_CT0QMS / 4] = V3D_3D_POOL_SIZE;
    core[CLE_CT0QTS / 4] = (1u << 1) | _tsda_p;   /* enable | page-aligned tsda */
    core[PTB_BPOS / 4]   = 0;                     /* kernel zeroes overspill */
    __asm__ __volatile__("dsb sy" ::: "memory");
    v3d_g2d_l2_invalidate_all();
    core[CLE_CT0QBA / 4] = start;
    core[CLE_CT0QEA / 4] = end;                   /* write starts the job */
    rc = wait_flush(core, CLE_BFC, before);
    if (rc != 0) {
        uint32_t mctl = 0, mvio = 0, mid = 0, mint = 0;
        int mf = v3d_g2d_mmu_fault_peek(&mctl, &mvio, &mid, &mint);
        slog("v3d_3d: BIN timeout bfc %u->%u intsts 0x%x ct0cs 0x%x ct1cs 0x%x bpos 0x%x cl [%u,%u)\r\n",
             before, core[CLE_BFC / 4], core[V3D_INT_STS / 4],
             core[CLE_CT0CS / 4], core[CLE_CT1CS / 4], core[PTB_BPOS / 4],
             start, end);
        /* bin control-thread position: CT0CA/CT0RA = where in the CL it is
         * stuck, CT0PC = primitives processed, ERR_STAT = core error latch. */
        slog("v3d_3d: BIN ct0ca 0x%x ct0ra 0x%x ct0lc 0x%x ct0pc 0x%x err 0x%x\r\n",
             core[CLE_CT0CA / 4], core[CLE_CT0RA / 4], core[CLE_CT0LC / 4],
             core[CLE_CT0PC / 4], core[V3D_ERR_STAT / 4]);
        /* MMU fault state: fault=1 + vio_id client 0x30-0x37=CLE / 0x38=PTB
         * proves the binner hit an unmapped IOVA (vio_addr); fault=0 means the
         * coord-shader QPU stalled with the mapping intact. */
        slog("v3d_3d: BIN mmu fault=%d ctl=0x%x vio_addr=0x%x vio_id=0x%x hub_int=0x%x\r\n",
             mf, mctl, mvio, mid, mint);
        /* Binner pool write cursor: bpca advanced past the pool base (_pool_p)
         * means the binner emitted tile lists (coord shader produced binnable
         * vertices); bpca == base means it wrote nothing.  err is shown
         * before->after with new = bits that appeared during THIS bin (sticky
         * bits already set before the ring are masked out), so a new bit12
         * (VCDI) proves this bin faulted rather than inheriting a stale latch. */
        slog("v3d_3d: BIN pool base 0x%x bpca 0x%x bpcs 0x%x err %x->%x new %x\r\n",
             _pool_p, core[PTB_BPCA / 4], core[PTB_BPCS / 4],
             err_before, core[V3D_ERR_STAT / 4],
             core[V3D_ERR_STAT / 4] & ~err_before);
        /* CT0 is stuck mid-job: reset the block so later dispatches (2D/CSD
         * included) do not wait out their budget behind it. */
        stalled("bin timeout");
    }
    return rc;
}

/* submit the render CL: drop stale cache lines, ring CT1, wait for the
 * render flush count. */
static int submit_render(volatile uint32_t *core, uint32_t start, uint32_t end)
{
    uint32_t before = core[CLE_RFC / 4];
    int rc;

    v3d_g2d_l2_invalidate_all();
    core[CLE_CT1CFG / 4] = 0;                     /* QCFG default */
    /* Clear a boot-time halt on the render CLE thread before ringing CT1.
     * The proven bare-metal bring-up (videocore7-exps/app/src/v3d.c,
     * v3d_submit_render) writes CT1CS = 0 at exactly this point; the EwokOS
     * port dropped it.  Without the write the render control thread stays
     * parked from boot, the CT1 job never advances and CLE_RFC never
     * increments, so wait_flush burns its whole deadline and every dispatch
     * (even the shader-less probe clear) degrades to V3D_3D_FAILED -- which
     * latches the client's GPU path off and drops shadertoy to the CPU. */
    core[CLE_CT1CS / 4] = 0;
    __asm__ __volatile__("dsb sy" ::: "memory");
    core[CLE_CT1QBA / 4] = start;
    core[CLE_CT1QEA / 4] = end;
    rc = wait_flush(core, CLE_RFC, before);
    if (rc != 0) {
        slog("v3d_3d: RENDER timeout rfc %u->%u intsts 0x%x ct0cs 0x%x ct1cs 0x%x cl [%u,%u)\r\n",
             before, core[CLE_RFC / 4], core[V3D_INT_STS / 4],
             core[CLE_CT0CS / 4], core[CLE_CT1CS / 4], start, end);
        stalled("render timeout");
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* empty BCL (clear path): produces the per-tile lists the RCL branches  */
/* to.  64px tiles to match build_clear_rcl and build_draw_bcl.          */
/* ------------------------------------------------------------------ */
static uint32_t build_bcl_empty(uint32_t w, uint32_t h)
{
    uint32_t off = 0;

    off = cl_u8(off, 0x77);                     /* NUMBER_OF_LAYERS */
    off = cl_u8(off, 0x00);                     /* layers - 1 = 0 */
    off = cl_u8(off, 0x78);                     /* TILE_BINNING_MODE_CFG */
    off = cl_u8(off, (1u << 4) | (0u << 2));    /* block 128b, initial 64b (matches RCL 0x7E) */
    /* 64px tiles (log2t=3), matching build_draw_bcl/build_draw_rcl.  The clear
     * bin previously used 16px tiles (log2t=1): at 1280x800 that is 80x50=4000
     * tiles, and the binner's per-tile allocation overran the 256 KiB pool
     * (bpca pinned at base+0x40000, bpcs 0) so the bin flush never landed and
     * v3d_3d_clear timed out -> pgl_g3d probe-clear failed -> GPU latched off
     * for the whole session.  64px tiles cut it to 20x13=260 tiles, the same
     * geometry the draw path already completes with. */
    off = cl_u8(off, (3u << 3) | (3u << 0));    /* log2 tile h/w = 3 (64px) */
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
 * to the framebuffer.  64px tiles (tile arg = 8<<log2t, log2t=3), matching
 * build_bcl_empty's binning and build_draw_rcl. */
static uint32_t build_clear_rcl(uint32_t fb_iova, uint32_t stride_bytes,
                                uint32_t w, uint32_t h, uint32_t tile,
                                uint32_t fmt, uint32_t rgba8, int rb_swap)
{
    uint32_t off = 0;
    uint32_t tx = (w + tile - 1) / tile;
    uint32_t ty = (h + tile - 1) / tile;
    uint32_t log2t = 3;             /* 64 px tiles (tile arg must be 8<<log2t) */
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
    /* cl[1]: bits4-5 = tile_allocation_block_size, bits2-3 =
     * tile_allocation_initial_block_size (mesa v3d_packet.xml v71).  The
     * draw bin previously emitted (0<<4)|(1<<2) = block 64b / initial 128b,
     * copied from the bare-metal reference's UNFINISHED draw path, while the
     * reference's clear bin -- the only binning config ever proven to
     * complete on this silicon -- uses (1<<4)|(0<<2) = block 128b / initial
     * 64b.  With the swapped pairing the tile allocator ran away (bpca
     * advanced ~20 KB for 80 tiles / 2 triangles) and the bin flush never
     * landed (bfc unmoved, no fresh ERR_STAT, no MMU fault).  Use the
     * proven-completing pairing; cl[2] keeps the 64px tile size to match
     * build_draw_rcl. */
    off = cl_u8(off, (1u << 4) | (0u << 2));    /* block 128b, initial 64b */
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
    /* TILE_LIST_INITIAL_BLOCK_SIZE (0x7E): auto-chain, first block 64b.
     * MUST match the binner's tile_allocation_initial_block_size in
     * TILE_BINNING_MODE_CFG (64b, bits 2-3 = 0): the render CLE walks each
     * tile's first block with this size, so 128b here made it run past the
     * 64b block the binner wrote into the neighbouring tile's list and fault
     * with CTERR (ct1cs 0x218) as soon as real geometry landed in a tile. */
    off = cl_u8(off, 0x7E);
    off = cl_u8(off, (1u << 2) | (0u << 0));
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
        /* SET_INSTANCEID (V3D71 length 5: opcode + u32 instance id; PTB
         * expects 0).  Emitting only one id byte made the CLE swallow the
         * following 0x15 0x00 0x1D bytes as the id and then fetch the STORE
         * packet's buffer byte (0x00) as the next opcode = HALT: render thread
         * stopped inside the generic list (ct1cs 0x110 = CTSUBS, RTSD 1,
         * CTRUN 0, no CTERR) and RFC never advanced. */
        gp[g++] = 0x36;
        gp[g++] = 0x00;
        gp[g++] = 0x00;
        gp[g++] = 0x00;
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
/* WS4 compiled-draw helpers: the mesa QUNIFORM interpreter + the           */
/* full-screen-quad attribute wiring.                                      */
/* ------------------------------------------------------------------ */

/* mesa's NIR->QPU compiler emits, per uniform slot, a `contents` descriptor
 * (enum quniform_contents, mesa/src/broadcom/compiler/v3d_compiler.h) plus a
 * 32-bit arg.  At draw time the gallium driver (v3d_set_uniform) expands each
 * slot into ONE u32 the QPU pulls with ldunifrf.  That driver is NOT vendored
 * here (only broadcom/{cle,common,compiler,qpu} are), so expand_uniforms()
 * mirrors its documented semantics for the subset the shadertoy full-screen-
 * quad path can produce.  These values are the leading entries of the enum and
 * are stable across the pinned mesa. */
#define QUNIFORM_CONSTANT          0u
#define QUNIFORM_UNIFORM           1u
#define QUNIFORM_VIEWPORT_X_SCALE  2u
#define QUNIFORM_VIEWPORT_Y_SCALE  3u
#define QUNIFORM_VIEWPORT_Z_OFFSET 4u
#define QUNIFORM_VIEWPORT_Z_SCALE  5u
#define QUNIFORM_TMU_CONFIG_P0     6u
#define QUNIFORM_TMU_CONFIG_P1     7u
#define QUNIFORM_UBO_ADDR          16u  /* unit 0 = default uniform blob */
#define QUNIFORM_SSBO_OFFSET       19u  /* data = SSBO binding; compute route */
#define QUNIFORM_SPILL_OFFSET          31u  /* data = byte offset into the scratch BO */
#define QUNIFORM_SPILL_SIZE_PER_THREAD 32u  /* value = bytes/thread of spill scratch */

static uint32_t f32_bits(float f)
{
    uint32_t u;
    __builtin_memcpy(&u, &f, 4);
    return u;
}

/* V3D 7.1 clipper XY granularity (mesa v3d_device_info.c: 64 for ver 71,
 * 256 for ver 42): Xs/Ys VPM words and CLIPPER_XY_SCALING are in 1/64 px. */
#define V3D_3D_CLIPPER_XY_GRANULARITY 64.0f

/* Expand one stage's uniform stream into `out` (num u32s).  contents[]/data[]
 * are the cached CPU descriptors+args (see v3d_3d_prog_t); job_unif (num_job
 * u32s, may be NULL) is the per-draw GL uniform storage a QUNIFORM_UNIFORM slot
 * indexes; vp_* are the viewport transform values (mesa's job->vp_xscale etc.).
 * ctex carries this draw's per-unit texture bindings (may be NULL): a
 * QUNIFORM_TMU_CONFIG_P0/P1 slot resolves its unit index (data >> 24) against
 * the matching pre-built state record in _texstate / _sampler.
 * Returns 0 on success, -1 if any slot's contents is outside the handled subset
 * or a QUNIFORM_UNIFORM index is out of range (the caller then declines the
 * draw rather than feeding the QPU a wrong value). */
static int expand_uniforms(uint32_t *out, const uint32_t *contents,
                           const uint32_t *data, uint32_t num,
                           const uint32_t *job_unif, uint32_t num_job,
                           uint32_t job_unif_iova,
                           const v3d_3d_ctex_t *ctex,
                           float vp_xscale, float vp_yscale,
                           float vp_zscale, float vp_zoff,
                           const uint32_t *ssbo_iova, uint32_t num_ssbo,
                           uint32_t spill_iova, uint32_t spill_size_per_thread)
{
    uint32_t i;
    for (i = 0; i < num; i++) {
        uint32_t c = contents[i] & 0xFFu;   /* the enum value (defensive mask) */
        uint32_t d = data[i];
        switch (c) {
        case QUNIFORM_CONSTANT:
            out[i] = d;                     /* the arg IS the value */
            break;
        case QUNIFORM_UNIFORM:
            if (job_unif == NULL || d >= num_job) {
                slog("v3d_3d: unif slot %u GL uniform[%u] out of %u -> decline\r\n",
                     i, d, num_job);
                return -1;                  /* per-draw GL uniform[d] */
            }
            out[i] = job_unif[d];
            break;
        case QUNIFORM_UBO_ADDR: {
            /* Indirect default-uniform loads carry a byte offset in the low
             * 24 bits and buffer index 0 in the high byte.  Real UBO units are
             * not exposed by this GLES route. */
            uint32_t unit = d >> 24;
            uint32_t off = d & 0x00FFFFFFu;
            if (unit != 0 || job_unif_iova == 0 || (off & 3u) != 0 ||
                off / 4u >= num_job) {
                slog("v3d_3d: unif slot %u UBO unit %u offset %u/%u -> decline\r\n",
                     i, unit, off, num_job * 4u);
                return -1;
            }
            out[i] = job_unif_iova + off;
            break;
        }
        /* Viewport uniforms are plain float32 (mesa cl_aligned_f): the
         * compiled VS/coord shader does nir_fmul(pos, load_viewport_x_scale)
         * then f2i32 to produce the Xs/Ys VPM words (v3d_nir_lower_io.c
         * v3d_nir_emit_ff_vpm_outputs).  The X/Y scale carries the clipper
         * granularity: 64 sub-pixel units on V3D 7.1 (v3d_device_info.c
         * clipper_xy_granularity; 256 on 4.2), matching the BCL CLIPPER_XY_
         * SCALING packet.  A packed half pair here reads as a ~1e17 float,
         * saturating every Xs/Ys: the frame completes but nothing is
         * rasterised. */
        case QUNIFORM_VIEWPORT_X_SCALE:
            out[i] = f32_bits(vp_xscale * V3D_3D_CLIPPER_XY_GRANULARITY);
            break;
        case QUNIFORM_VIEWPORT_Y_SCALE:
            out[i] = f32_bits(vp_yscale * V3D_3D_CLIPPER_XY_GRANULARITY);
            break;
        case QUNIFORM_VIEWPORT_Z_SCALE:
            out[i] = f32_bits(vp_zscale);
            break;
        case QUNIFORM_VIEWPORT_Z_OFFSET:
            out[i] = f32_bits(vp_zoff);
            break;
        case QUNIFORM_TMU_CONFIG_P0: {
            /* A compiled FS that samples iChannelN emits a TMU_CONFIG_P0/P1
             * pair per unit.  data = (unit << 24) | low; the gallium driver
             * ORs `low` into the matching hardware state record's IOVA.  The
             * texture records were built for this draw by
             * build_tex_state_records() into _texstate (32-byte slots, so bits
             * [3:0] are free for return_words_of_texture_data).  Decline if the
             * unit is unbound/out of range so a mis-wired FS falls back to CPU
             * rather than sampling a stale or zero record. */
            uint32_t unit = d >> 24;
            if (ctex == NULL || unit >= V3D_3D_MAX_TEX_UNITS ||
                ctex->ctex_iova[unit] == 0) {
                slog("v3d_3d: unif slot %u TMU P0 unit %u unbound -> decline\r\n", i, unit);
                return -1;
            }
            out[i] = (_texstate_p + unit * V3D_3D_TEXSTATE_SLOT) | (d & 0xFu);
            break;
        }
        case QUNIFORM_TMU_CONFIG_P1: {
            /* Same unit resolution; the sampler records are identical for every
             * shadertoy texture (GL_NEAREST + GL_REPEAT) and built once at init.
             * bits [2:0] carry per_pixel_mask / unnormalized / output_type_32. */
            uint32_t unit = d >> 24;
            if (ctex == NULL || unit >= V3D_3D_MAX_TEX_UNITS ||
                ctex->ctex_iova[unit] == 0) {
                slog("v3d_3d: unif slot %u TMU P1 unit %u unbound -> decline\r\n", i, unit);
                return -1;
            }
            out[i] = (_sampler_p + unit * V3D_3D_TEXSTATE_SLOT) | (d & 0x7u);
            break;
        }
        case QUNIFORM_SSBO_OFFSET:
            /* Compute route: mesa resolves this slot to a bound SSBO's GPU
             * address.  The compiler already remapped the slot data from the
             * linker block index to the declared BINDING, so index the caller's
             * ssbo_iova[] table by it.  The shadertoy route passes a 1-entry
             * table (binding 0 = fb); the geometry rasterizer passes 4
             * (fb/pos/nrm/depth). */
            if (ssbo_iova == NULL || d >= num_ssbo || ssbo_iova[d] == 0) {
                slog("v3d_3d: unif slot %u SSBO binding %u unbound -> decline\r\n", i, d);
                return -1;
            }
            out[i] = ssbo_iova[d];
            break;
        case QUNIFORM_SPILL_OFFSET:
            /* Register-spill scratch base.  v3d_setup_spill_base() addresses a
             * thread's scratch as TIDX*SPILL_SIZE_PER_THREAD + (EIDX<<2) +
             * SPILL_OFFSET, so this slot is the scratch DMA base IOVA plus the
             * slot's byte offset (data, normally 0).  mesa sizes the BO
             * qpu_count*4*spill_size for TIDX = (core<<6)|(qpu<<2)|thread; the
             * daemon allocated it at program upload (cs_spill_iova).  Decline if
             * the kernel spills but no scratch was staged. */
            if (spill_iova == 0) {
                slog("v3d_3d: unif slot %u SPILL_OFFSET but no scratch -> decline\r\n", i);
                return -1;
            }
            out[i] = spill_iova + d;
            break;
        case QUNIFORM_SPILL_SIZE_PER_THREAD:
            /* The per-thread scratch stride in bytes (== prog_data->spill_size),
             * a plain integer (not float bits). */
            if (spill_size_per_thread == 0) {
                slog("v3d_3d: unif slot %u SPILL_SIZE_PER_THREAD 0 -> decline\r\n", i);
                return -1;
            }
            out[i] = spill_size_per_thread;
            break;
        default:
            /* Blend-constant / line-width / ... : the full-screen-quad
             * shadertoy path never emits these, so decline
             * rather than guess an encoding.  Log the slot so a device run
             * names the missing QUNIFORM type instead of a bare -2. */
            slog("v3d_3d: unif slot %u type %u data 0x%x unhandled -> decline\r\n",
                 i, c, d);
            return -1;
        }
    }
    return 0;
}

/* Compiled-draw decline: name the gate so the client's bare -2 is
 * attributable from /dev/log (the client latches to CPU after the first). */
static int decline(const char *why)
{
    slog("v3d_3d: compiled draw declined: %s\r\n", why);
    return V3D_3D_UNSUPPORTED;
}

/* Compute-route draw: the program's fragment stage was recompiled as a compute
 * kernel (pglc_compile_compute) whose SSBO binding 0 is the render target and
 * whose per-draw blob carries `pglc_cs_stride` (dwords per row) alongside the
 * effect's own uniforms.  One invocation per pixel: the grid is
 * ceil(w/lx) x ceil(h/ly) x 1 and the kernel clips x>=w / y>=h itself from
 * iResolution.  Serialized against the 2D CSD kernels by the dispatch lock
 * inside v3d_g2d_run_csd; the frame buffer is contig-shm / dma (NOCACHE on
 * the ARM side), so GPU-side L2T invalidate pre / L2 writeback post is the
 * whole coherency story. */
static int draw_compute(const v3d_3d_prog_t *pr, uint32_t fb_iova,
                        uint32_t stride_bytes, uint32_t fmt,
                        uint32_t w, uint32_t h,
                        const uint32_t *unif, uint32_t num_uniforms,
                        const v3d_3d_ctex_t *ctex)
{
    v3d_g2d_csd_job_t job;
    int rc;

    if (fmt != V3D_3D_FMT_RGBA8)
        return decline("compute: fmt not RGBA8");
    if ((stride_bytes & 3u) != 0 || stride_bytes < w * 4u)
        return decline("compute: stride");
    if (pr->cs_local_size[0] == 0 || pr->cs_local_size[1] == 0 ||
        pr->cs_local_size[2] != 1u)
        return decline("compute: local_size");
    if (pr->cs_shared_size != 0)
        return decline("compute: shared memory unsupported");

    v3d_g2d_op_begin();
    if (num_uniforms > V3D_3D_JOB_MAX_UNIF ||
        (num_uniforms != 0 && unif == NULL)) {
        v3d_g2d_op_end();
        return decline("compute: uniform blob");
    }
    if (num_uniforms != 0)
        memcpy(_job_unif_va, unif, num_uniforms * 4u);
    if (build_tex_state_records(ctex) != 0) {
        v3d_g2d_op_end();
        return decline("compute: ctex base not 64B aligned");
    }
    if (expand_uniforms(_ccu_unif_va, pr->cs_unif_contents, pr->cs_unif_data,
                        pr->cs_num_uniforms, unif, num_uniforms, _job_unif_p,
                        ctex, 0.0f, 0.0f, 0.0f, 0.0f, &fb_iova, 1u,
                        pr->cs_spill_iova, pr->cs_spill_size) != 0) {
        v3d_g2d_op_end();
        return decline("compute: uniforms");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");

    memset(&job, 0, sizeof(job));
    job.code_iova   = pr->cs_qpu_iova;
    job.unif_iova   = _ccu_unif_p;
    job.wg_count[0] = (w + pr->cs_local_size[0] - 1u) / pr->cs_local_size[0];
    job.wg_count[1] = (h + pr->cs_local_size[1] - 1u) / pr->cs_local_size[1];
    job.wg_count[2] = 1u;
    job.wg_size     = pr->cs_local_size[0] * pr->cs_local_size[1];
    job.threads     = pr->cs_threads;
    job.single_seg  = pr->cs_single_seg;
    job.dst_phy     = fb_iova;
    job.dst_span    = stride_bytes * h;
    job.unif_span   = pr->cs_num_uniforms * 4u;
    job.src_phy     = _job_unif_p;       /* ARM-written indirect uniform data */
    job.src_span    = num_uniforms * 4u;
    job.tag         = V3D_G2D_TAG_COMPUTE;
    /* a ray-marching effect at 640x480 legitimately runs for hundreds of ms;
     * give it a real budget instead of the 2D fail-fast default */
    job.budget_ms   = V3D_3D_COMPUTE_BUDGET_MS;
    rc = v3d_g2d_run_csd(&job, V3D_G2D_MAINT_ALL | V3D_G2D_POLL_YIELD |
                               V3D_G2D_POLL_IRQ);
    v3d_g2d_op_end();
    if (rc != 0) {
        slog("v3d_3d: compute draw rc=%d grid %ux%u wg=%u thr=%u unif=%u\r\n",
             rc, job.wg_count[0], job.wg_count[1], job.wg_size,
             (uint32_t)pr->cs_threads, pr->cs_num_uniforms);
        return rc > 0 ? V3D_3D_TIMEOUT : V3D_3D_MMU_FAULT;
    }
    return V3D_3D_DONE;
}

/* Geometry compute route (geom_raster_cs.h), TWO-PASS: vertex shading is
 * hoisted out of the rasterizer so each vertex is transformed+lit exactly once
 * instead of once per triangle-workgroup-invocation (a 64x ALU cut for the
 * teapot).  Both passes are CSD dispatches under ONE op bracket (the dispatch
 * lock is owner-recursive) so no other worker can interleave and clobber the
 * shared `vout` intermediate between them.
 *
 *   Pass 1 - cs2 (xform): grid ceil(nverts/64) workgroups of 64, one invocation
 *     per vertex.  SSBO 0 = vout (8-float/32-byte records), 1 = positions
 *     (float vec3, 12-byte) or interleaved pos+uv (6-float) for the textured
 *     variant, 2 = normals (gouraud only).  dst = vout so its POST writeback
 *     cleans the records to DRAM for pass 2 to read; its PRE is ranged over
 *     the vertex streams (the only ARM-written surfaces it reads).
 *   Pass 2 - cs (raster): ONE workgroup per triangle, local_size 8x8.  SSBO
 *     0 = fb, 1 = vout, 2 = depth (gouraud) / 3 = texture (textured).  dst = fb
 *     + aux = depth for the POST writeback.  It runs with NO PRE: vout, fb
 *     and depth are all GPU-owned (the fills and pass 1 just wrote them, the
 *     previous frame's POST cleaned them); the textured variant keeps a
 *     ranged PRE over the texture image, which the ARM wrote at upload.
 *
 * The frame's first draw also clears colour + depth on the GPU (two
 * write-only fills with no maintenance of their own), replacing the
 * client's per-frame CPU clear of both canvases through NOCACHE shm.
 *
 * The per-draw uniform blob is the concatenation [cs2/xform | cs/raster]; the
 * split is the program's cs2_uniform_blob_dwords. */
static int draw_compute_geom(const v3d_3d_prog_t *pr, uint32_t fb_iova,
                             uint32_t stride_bytes, uint32_t fmt,
                             uint32_t w, uint32_t h, uint32_t nverts,
                             uint32_t pos_iova, uint32_t nrm_iova,
                             uint32_t depth_iova, uint32_t z_stride,
                             const uint32_t *unif, uint32_t num_uniforms,
                             uint32_t tex_iova, uint32_t tex_span,
                             uint32_t inst_iova, uint32_t inst_span, int is_inst,
                             int is_texcol, int first_frame, uint32_t clear_rgba)
{
    v3d_g2d_csd_job_t job;
    uint32_t ssbo[4];
    uint32_t ntris = nverts / 3u;
    uint32_t split = pr->cs2_uniform_blob_dwords;
    uint32_t xwg;
    int rc;

    if (fmt != V3D_3D_FMT_RGBA8)
        return decline("geom: fmt not RGBA8");
    if ((stride_bytes & 3u) != 0 || stride_bytes < w * 4u)
        return decline("geom: stride");
    if ((z_stride & 3u) != 0 || z_stride < w * 4u)
        return decline("geom: depth stride");
    if (ntris == 0 || ntris >= (1u << 16))
        return decline("geom: triangle count");
    if (nverts > V3D_3D_MAX_VERTS)
        return decline("geom: nverts > vout");
    if (pr->cs_local_size[0] == 0 || pr->cs_local_size[1] == 0 ||
        pr->cs_local_size[2] != 1u)
        return decline("geom: local_size");
    if (pr->cs_shared_size != 0)
        return decline("geom: shared memory unsupported");
    /* pass 1 (cs2/xform) is mandatory on the two-pass geom route */
    if (pr->cs2_qpu_words == 0 ||
        pr->cs2_local_size[0] == 0 || pr->cs2_local_size[1] == 0 ||
        pr->cs2_local_size[2] != 1u)
        return decline("geom: xform stage missing");
    if (pr->cs2_shared_size != 0)
        return decline("geom: xform shared memory unsupported");
    if (split > num_uniforms)
        return decline("geom: uniform blob shorter than xform region");
    /* The daemon-side frame clear fills tightly-packed rows only; a pitched
     * colour or depth canvas would be left half-cleared, so decline it and
     * the client keeps its own CPU clear (pgl_g3d always sends w*4). */
    if (first_frame && (stride_bytes != w * 4u || z_stride != w * 4u))
        return decline("geom: first_frame pitched strides");

    v3d_g2d_op_begin();

    /* Frame clear on the GPU, before either pass: the client drops its
     * per-frame CPU clear of colour+depth (~2.4 MB of NOCACHE writes) once
     * this path has run, so the frame's first geom draw fills both canvases
     * itself.  The fills are write-only and take no L2T maintenance of
     * their own: pass 1's POST drains the write combiner before pass 2's
     * depth atomics read, and pass 2's POST writes fb+depth back to DRAM.
     * The geom fb layout is 0xAARRGGBB while clear_rgba is the back end's
     * 0xAABBGGRR, so R and B swap. */
    if (first_frame) {
        uint32_t c = clear_rgba;
        uint32_t px = (c & 0xFF000000u) | ((c & 0x000000FFu) << 16) |
                      (c & 0x0000FF00u) | ((c & 0x00FF0000u) >> 16);

        if (!gpu_fill_surface_m(fb_iova, NULL, (int32_t)w, (int32_t)h,
                                0, 0, (int32_t)w, (int32_t)h, px, 0u) ||
            !gpu_fill_surface_m(depth_iova, NULL, (int32_t)w, (int32_t)h,
                                0, 0, (int32_t)w, (int32_t)h,
                                0xFFFFFFFFu, 0u)) {
            v3d_g2d_op_end();
            return decline("geom: frame clear fill");
        }
    }

    /* ---- Pass 1: vertex transform -> vout ---------------------------- */
    ssbo[0] = _vout_p;
    ssbo[1] = pos_iova;
    if (tex_iova != 0u) {
        /* textured xform reads interleaved pos+uv from binding 1 only */
        if (expand_uniforms(_ccu_unif_va, pr->cs2_unif_contents, pr->cs2_unif_data,
                            pr->cs2_num_uniforms, unif, num_uniforms, 0u, NULL,
                            0.0f, 0.0f, 0.0f, 0.0f, ssbo, 2u, 0u, 0u) != 0) {
            v3d_g2d_op_end();
            return decline("geom: xform uniforms (tex)");
        }
    } else if (is_inst) {
        /* instanced xform: binding 2 = normals, binding 3 = the per-instance
         * matrix SSBO (gears) the client refills every frame. */
        ssbo[2] = nrm_iova;
        ssbo[3] = inst_iova;
        if (expand_uniforms(_ccu_unif_va, pr->cs2_unif_contents, pr->cs2_unif_data,
                            pr->cs2_num_uniforms, unif, num_uniforms, 0u, NULL,
                            0.0f, 0.0f, 0.0f, 0.0f, ssbo, 4u, 0u, 0u) != 0) {
            v3d_g2d_op_end();
            return decline("geom: xform uniforms (inst)");
        }
    } else {
        ssbo[2] = nrm_iova;
        if (expand_uniforms(_ccu_unif_va, pr->cs2_unif_contents, pr->cs2_unif_data,
                            pr->cs2_num_uniforms, unif, num_uniforms, 0u, NULL,
                            0.0f, 0.0f, 0.0f, 0.0f, ssbo, 3u, 0u, 0u) != 0) {
            v3d_g2d_op_end();
            return decline("geom: xform uniforms");
        }
    }
    __asm__ __volatile__("dsb sy" ::: "memory");

    xwg = pr->cs2_local_size[0] * pr->cs2_local_size[1];
    memset(&job, 0, sizeof(job));
    job.code_iova   = pr->cs2_qpu_iova;
    job.unif_iova   = _ccu_unif_p;
    job.wg_count[0] = (nverts + xwg - 1u) / xwg;   /* one invocation per vertex */
    job.wg_count[1] = 1u;
    job.wg_count[2] = 1u;
    job.wg_size     = xwg;
    job.threads     = pr->cs2_threads;
    job.single_seg  = pr->cs2_single_seg;
    job.dst_phy     = _vout_p;          /* POST writeback: vout -> DRAM */
    job.dst_span    = nverts * (is_texcol ? 48u : 32u);
    job.unif_span   = pr->cs2_num_uniforms * 4u;
    job.tag         = V3D_G2D_TAG_GEOM_XFORM;
    /* Ranged PRE: the only ARM-written surfaces this pass reads are the
     * vertex streams (vout is write-only here), so the hull of pos(+nrm)
     * bounds the invalidate instead of the whole IOVA range. */
    {
        uint32_t lo = pos_iova;
        uint32_t hi = pos_iova + nverts * (is_texcol ? 40u :
                                             (tex_iova != 0u ? 24u :
                                              (is_inst ? 16u : 12u)));

        if (tex_iova == 0u && nrm_iova != 0u) {
            uint32_t nlo = nrm_iova;
            uint32_t nhi = nrm_iova + nverts * 12u;

            if (nlo < lo) lo = nlo;
            if (nhi > hi) hi = nhi;
        }
        if (is_inst && inst_iova != 0u) {
            /* the per-frame instance-matrix buffer is ARM-written, so it joins
             * the PRE hull (drop any stale L2 lines from the previous frame). */
            uint32_t ghi = inst_iova + inst_span;

            if (inst_iova < lo) lo = inst_iova;
            if (ghi > hi) hi = ghi;
        }
        job.src_phy  = lo;
        job.src_span = hi - lo;
    }
    job.budget_ms   = V3D_3D_COMPUTE_BUDGET_MS;
    rc = v3d_g2d_run_csd(&job, V3D_G2D_MAINT_ALL | V3D_G2D_POLL_YIELD |
                               V3D_G2D_POLL_IRQ);
    if (rc != 0) {
        v3d_g2d_op_end();
        slog("v3d_3d: geom xform rc=%d nverts %u wg=%u thr=%u unif=%u "
             "ssbo[vout=%x pos=%x s2=%x s3=%x]\r\n",
             rc, nverts, job.wg_size, (uint32_t)pr->cs2_threads,
             pr->cs2_num_uniforms,
             ssbo[0], ssbo[1], ssbo[2], ssbo[3]);
        return rc > 0 ? V3D_3D_TIMEOUT : V3D_3D_MMU_FAULT;
    }

    /* ---- Pass 2: rasterize from vout --------------------------------- */
    ssbo[0] = fb_iova;
    ssbo[1] = _vout_p;
    ssbo[2] = depth_iova;
    if (tex_iova != 0u)
        ssbo[3] = tex_iova;
    /* the raster kernel's uniforms live in the SECOND region of the blob */
    if (expand_uniforms(_ccu_unif_va, pr->cs_unif_contents, pr->cs_unif_data,
                        pr->cs_num_uniforms, unif + split, num_uniforms - split,
                        0u, NULL, 0.0f, 0.0f, 0.0f, 0.0f,
                        ssbo, tex_iova != 0u ? 4u : 3u, 0u, 0u) != 0) {
        v3d_g2d_op_end();
        return decline("geom: raster uniforms");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");

    memset(&job, 0, sizeof(job));
    job.code_iova   = pr->cs_qpu_iova;
    job.unif_iova   = _ccu_unif_p;
    /* Raster grid: the legacy kernels map one workgroup to one triangle
     * (cs_geom_tpw 0/1); the one-lane-per-triangle kernels (V4/V5) cover
     * cs_geom_tpw triangles per workgroup, so the grid is ceil(ntris/tpw).
     * The wide grid is the whole performance point: the CSD charges ~3us
     * of fixed dispatch overhead PER WORKGROUP on this part (measured on
     * device: 6400 workgroups cost ~19-20ms no matter what the kernel
     * does), so ntris/256 workgroups is what makes the compute rasterizer
     * fast.  An over-dispatching peer (tpw mismatch) still renders
     * correctly through the kernel's uTriCount guard, just slowly. */
    {
        uint32_t tpw = pr->cs_geom_tpw;
        if (tpw == 0u)
            tpw = 1u;
        job.wg_count[0] = (ntris + tpw - 1u) / tpw;
    }
    job.wg_count[1] = 1u;
    job.wg_count[2] = 1u;
    job.wg_size     = pr->cs_local_size[0] * pr->cs_local_size[1];
    job.threads     = pr->cs_threads;
    job.single_seg  = pr->cs_single_seg;
    job.dst_phy     = fb_iova;
    job.dst_span    = stride_bytes * h;
    job.aux_phy     = depth_iova;   /* atomicMin depth canvas writeback */
    job.aux_span    = z_stride * h;
    job.unif_span   = pr->cs_num_uniforms * 4u;
    job.tag         = V3D_G2D_TAG_GEOM_RASTER;
    if (tex_iova != 0u) {
        job.src_phy  = tex_iova;    /* ARM-written at upload: ranged PRE */
        job.src_span = tex_span;
    }
    job.budget_ms   = V3D_3D_COMPUTE_BUDGET_MS;
    /* No PRE for the gouraud variant: every surface this pass touches
     * (vout, fb, depth) is GPU-owned and coherent through the L2T - the
     * fills and pass 1 just wrote them, and the previous frame's POST
     * cleaned them.  The textured variant keeps the ranged texture PRE. */
    rc = v3d_g2d_run_csd(&job, (tex_iova != 0u ? V3D_G2D_MAINT_PRE : 0u) |
                               V3D_G2D_MAINT_POST | V3D_G2D_POLL_YIELD |
                               V3D_G2D_POLL_IRQ);
    v3d_g2d_op_end();
    if (rc != 0) {
        slog("v3d_3d: geom raster rc=%d tris %u wg=%u thr=%u unif=%u "
             "ssbo[fb=%x vout=%x depth=%x tex=%x]\r\n",
             rc, ntris, job.wg_size,
             (uint32_t)pr->cs_threads, pr->cs_num_uniforms,
             ssbo[0], ssbo[1], ssbo[2], ssbo[3]);
        return rc > 0 ? V3D_3D_TIMEOUT : V3D_3D_MMU_FAULT;
    }
    return V3D_3D_DONE;
}

/* Compiled full-screen-quad attribute wiring: only attr0 = pos (float3, stride
 * 12).  The mesa-compiled VS reads clip-space position from VPM input in[0..2]
 * (driver location 0); a shadertoy quad carries no per-vertex colour or uv (the
 * FS derives colour procedurally from iResolution / iTime), so nattrs == 1 and
 * the second attribute record slot stays zeroed.  Written at _ssr + 32,
 * contiguous after the 32-byte shader record (the CLE walks nattrs records from
 * shader_record_address + 32).  Reuses the proven pos float3 encoding. */
static void build_attr_records_compiled(uint32_t pos_iova)
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
}

/* Compiled two-attribute variant: attr 0 = pos (float3), attr 1 = normal
 * (float3), BOTH tightly packed at stride 12.  This mirrors the proven
 * build_attr_records_tex float3 encoding (vec_size=3, type=FLOAT, nvals
 * vs=3/cs=3, stride 12) so the compiled VS reads in[0..2]=pos.xyz and
 * in[3..5]=normal.xyz against driver_location 0 and 1.  The mesh is staged
 * de-interleaved (structure-of-arrays: a pos block followed by a normal
 * block) rather than interleaved, so each attribute is its own contiguous
 * stride-12 stream and no per-vertex stride override is needed - the daemon
 * resolves pos_iova and normal_iova (col_iova) to two separate physical
 * bases.  Sub-range draws offset each base by first*12. */
static void build_attr_records_compiled2(uint32_t pos_iova, uint32_t normal_iova)
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
    a[16] = (uint8_t)(normal_iova & 0xFF);
    a[17] = (uint8_t)((normal_iova >> 8) & 0xFF);
    a[18] = (uint8_t)((normal_iova >> 16) & 0xFF);
    a[19] = (uint8_t)((normal_iova >> 24) & 0xFF);
    a[20] = (3u << 0) | (2u << 2);        /* vec_size=3, type=FLOAT */
    a[21] = (3u << 4) | (3u << 0);        /* nvals vs=3, cs=3 */
    a[24] = 12;                           /* stride = 12 bytes */
    a[28] = 0xFF; a[29] = 0xFF; a[30] = 0xFF; a[31] = 0xFF;  /* max index */
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
    if (v3d_g2d_wedged())
        return V3D_3D_UNSUPPORTED;   /* dead block: client stays on CPU */
    if (w == 0 || h == 0 || fb_iova == 0)
        return V3D_3D_UNSUPPORTED;
    if (!v3d_g2d_phy_valid(fb_iova, (size_t)stride_bytes * h))
        return V3D_3D_UNSUPPORTED;

    core = v3d_g2d_core_regs();
    v3d_g2d_op_begin();
    snprintf(_job_desc, sizeof(_job_desc), "clear %ux%u fmt %u", w, h, fmt);

    /* bin: empty frame -> per-tile lists */
    len = build_bcl_empty(w, h);
    if (len > V3D_3D_CL_MAX)
        goto out;
    rc = submit_bin(core, _cl_p, _cl_p + len);
    if (rc != 0)
        goto out;

    /* render: clear via the explicit per-tile store.  64px tiles to match the
     * 64px binning above (build_bcl_empty) and build_draw_rcl; a 16px clear at
     * 1280x800 overruns the 256 KiB binner pool and times out. */
    len = build_clear_rcl(fb_iova, stride_bytes, w, h, 64, fmt, rgba8, rb_swap);
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
                const v3d_3d_tex_t *tex,
                int32_t prog_handle, const uint32_t *unif,
                uint32_t num_uniforms,
                const v3d_3d_ctex_t *ctex)
{
    volatile uint32_t *core;
    uint32_t len;
    float fx, fy;
    uint32_t ux, uy;
    uint32_t sr_nattrs;
    int rc, ret = V3D_3D_FAILED;
    int compiled = (prog_handle >= 0);
    int tex_path = (!compiled && tex != NULL && tex->tex_iova != 0 &&
                    tex->uv_iova != 0 && tex->tex_w != 0 && tex->tex_h != 0);

    /* Compute route: a program carrying a compute stage never touches the
     * binner, so it is dispatched ahead of the interim binner gate below. */
    if (v3d_g2d_wedged())
        return V3D_3D_UNSUPPORTED;   /* dead block, no reset: quiet CPU fallback */
    if (compiled && _ready && (uint32_t)prog_handle < V3D_3D_MAX_PROGS &&
        _progs[prog_handle].used && _progs[prog_handle].cs_qpu_words != 0) {
        if (w == 0 || h == 0 || fb_iova == 0)
            return decline("compute: zero w/h/fb");
        if (!v3d_g2d_phy_valid(fb_iova, (size_t)stride_bytes * h))
            return decline("compute: fb iova");
        if (_progs[prog_handle].cs_is_geom) {
            /* geometry rasterizer: triangles only; the staged streams and the
             * depth canvas ride the draw's own iovas.  cs_is_geom == 2 is the
             * TEXTURED kernel: pos_iova carries interleaved pos+uv (6-float
             * stride, nverts*24 bytes), col_iova is unused (zero canvas), and
             * the texture image arrives as ctex unit 0 (linear uint32).
             * cs_is_geom == 3 is the INSTANCED kernel (gears): pos_iova carries
             * vec4 pos+instance-index (16-byte stride), col_iova the float3
             * normals, and ctex unit 0 the per-frame instance-matrix SSBO.
             * cs_is_geom == 4 is the COLOUR-MODULATED TEXTURED kernel (imgui):
             * pos_iova carries interleaved pos+uv+rgba (10-float stride,
             * nverts*40 bytes), col_iova is unused (zero canvas), ctex unit 0
             * the linear font-atlas image; the raster kernel alpha-blends and
             * clips (uScissor) with no depth write. */
            int gtex = (_progs[prog_handle].cs_is_geom == 2);
            int ginst = (_progs[prog_handle].cs_is_geom == 3);
            int gtc = (_progs[prog_handle].cs_is_geom == 4);
            /* textured family (plain tex + colour-modulated tex): col unused,
             * ctex[0] is the linear image the raster kernel indexes directly. */
            int gtexc = gtex || gtc;
            uint32_t pstride = gtc ? 40u : (gtex ? 24u : (ginst ? 16u : 12u));
            if (mode != V3D_3D_MODE_TRIANGLES)
                return decline("geom: mode not triangles");
            if (nverts == 0 || (nverts % 3u) != 0)
                return decline("geom: nverts not multiple of 3");
            if (!v3d_g2d_phy_valid(pos_iova, (size_t)nverts * pstride))
                return decline("geom: pos iova");
            if (!gtexc && !v3d_g2d_phy_valid(col_iova, (size_t)nverts * 12u))
                return decline("geom: nrm iova");
            if (gtexc && (ctex == NULL || ctex->ctex_iova[0] == 0))
                return decline("geom tex: no texture bound");
            if (gtexc && (ctex->ctex_stride[0] < ctex->ctex_w[0] * 4u ||
                !v3d_g2d_phy_valid(ctex->ctex_iova[0],
                                   (size_t)ctex->ctex_stride[0] * ctex->ctex_h[0])))
                return decline("geom tex: tex iova");
            if (ginst && (ctex == NULL || ctex->ctex_iova[0] == 0))
                return decline("geom inst: no instance buffer bound");
            if (ginst && (ctex->ctex_stride[0] < ctex->ctex_w[0] * 4u ||
                !v3d_g2d_phy_valid(ctex->ctex_iova[0],
                                   (size_t)ctex->ctex_stride[0] * ctex->ctex_h[0])))
                return decline("geom inst: instance iova");
            if (z_iova == 0 ||
                !v3d_g2d_phy_valid(z_iova, (size_t)z_stride * h))
                return decline("geom: depth iova");
            return draw_compute_geom(&_progs[prog_handle], fb_iova,
                                     stride_bytes, fmt, w, h, nverts,
                                     pos_iova, col_iova, z_iova, z_stride,
                                     unif, num_uniforms,
                                     gtexc ? ctex->ctex_iova[0] : 0u,
                                     gtexc ? ctex->ctex_stride[0] *
                                             ctex->ctex_h[0] : 0u,
                                     ginst ? ctex->ctex_iova[0] : 0u,
                                     ginst ? ctex->ctex_stride[0] *
                                             ctex->ctex_h[0] : 0u,
                                     ginst, gtc, first_frame, clear_rgba);
        }
        return draw_compute(&_progs[prog_handle], fb_iova, stride_bytes, fmt,
                            w, h, unif, num_uniforms, ctex);
    }

    /* Interim: the binner geometry path is unproven on this silicon and a
     * timed-out bin wedges the shared V3D (desktop corruption until reboot).
     * Decline compiled draws before touching the pipeline; see the gate
     * comment at V3D_3D_ENABLE_COMPILED_DRAW. */
    if (compiled && !V3D_3D_ENABLE_COMPILED_DRAW)
        return decline("gate");
    if (!compiled && !V3D_3D_ENABLE_FIXED_DRAW)
        return V3D_3D_UNSUPPORTED;   /* unproven shaders: client draws on CPU */

    if (!_ready)
        return compiled ? decline("not ready") : V3D_3D_UNSUPPORTED;
    if (w == 0 || h == 0 || fb_iova == 0 || nverts == 0)
        return compiled ? decline("zero w/h/fb/nverts") : V3D_3D_UNSUPPORTED;
    if (nverts > V3D_3D_MAX_VERTS)
        return compiled ? decline("nverts > max") : V3D_3D_UNSUPPORTED;
    if (mode != V3D_3D_MODE_POINTS && mode != V3D_3D_MODE_LINES &&
        mode != V3D_3D_MODE_TRIANGLES)
        return compiled ? decline("mode") : V3D_3D_UNSUPPORTED;
    /* a compiled draw must reference a live cache slot; reject a stale or
     * out-of-range handle before touching the pipeline. */
    if (compiled && ((uint32_t)prog_handle >= V3D_3D_MAX_PROGS ||
                     !_progs[prog_handle].used))
        return decline("prog handle not live");
    if (!v3d_g2d_phy_valid(fb_iova, (size_t)stride_bytes * h))
        return compiled ? decline("fb iova") : V3D_3D_UNSUPPORTED;
    if (!v3d_g2d_phy_valid(pos_iova, (size_t)nverts * 12u))
        return compiled ? decline("pos iova") : V3D_3D_UNSUPPORTED;
    /* col_iova is not read on the tex path (attr slot 1 carries uv instead);
     * uv float3 shares the pos encoding's 12-byte stride.  On the compiled path
     * col_iova is either 0 (pos-only full-screen quad, e.g. shadertoy) or the
     * attr-1 normal float3 stream of a de-interleaved mesh (stride 12), so it
     * is validated as nverts*12 only when present.  The fixed colour path keeps
     * its byte4 (nverts*4) validation. */
    if (tex_path) {
        if (!v3d_g2d_phy_valid(tex->uv_iova, (size_t)nverts * 12u))
            return V3D_3D_UNSUPPORTED;
        if (tex->tex_stride < tex->tex_w * 4u)
            return V3D_3D_UNSUPPORTED;
        if (!v3d_g2d_phy_valid(tex->tex_iova,
                               (size_t)tex->tex_stride * tex->tex_h))
            return V3D_3D_UNSUPPORTED;
    } else if (compiled) {
        if (col_iova != 0 &&
            !v3d_g2d_phy_valid(col_iova, (size_t)nverts * 12u))
            return decline("col/normal iova");
    } else if (!v3d_g2d_phy_valid(col_iova, (size_t)nverts * 4u)) {
        return V3D_3D_UNSUPPORTED;
    }
    /* a persistent Z surface is optional (z_iova == 0 -> tile-local Z only);
     * when present it must be a valid Z32F linear surface covering the frame */
    if (z_iova != 0 && !v3d_g2d_phy_valid(z_iova, (size_t)z_stride * h))
        return compiled ? decline("z iova") : V3D_3D_UNSUPPORTED;

    /* viewport transform values shared by every path: x scale = w/2, y scale =
     * -h/2 (the target is y-down).  The fixed vertex + coord shaders read these
     * as float32 uniforms; the compiled path hands them to the mesa VS as
     * float32 too, scaled by the 7.1 clipper granularity (expand_uniforms). */
    fx = (float)(w / 2u);
    fy = -(float)(h / 2u);
    __builtin_memcpy(&ux, &fx, 4);
    __builtin_memcpy(&uy, &fy, 4);
    /* the coordinate (binning) shader is our own hand-written pass-through and
     * keeps its float32 viewport ABI on every path, including compiled draws
     * (the binner only reads clip-space pos). */
    _cs_unif_va[0] = 0x3F800000u;               /* coord: 1.0 (w) */
    _cs_unif_va[1] = ux;                        /* coord: vp x scale */
    _cs_unif_va[2] = uy;                        /* coord: vp y scale */

    if (compiled) {
        /* Compiled draw (WS3): expand the mesa VS / FS / COORD uniform streams
         * into their shared per-draw dma staging, then build the shader-state
         * record from the cached QPU IOVAs.  The coordinate (binning) shader is
         * now the program's OWN compiled is_coord VS (pr->coord_qpu_iova) rather
         * than the fixed pass-through, so the binner applies the real MVP and
         * arbitrary geometry bins correctly; its uniforms expand from the SAME
         * per-draw blob (the coord stage shares the render VS's driver_locations,
         * so e.g. mvp lands at the identical dword offset).  The VPM segment
         * bytes come from the cached coord/vs prog_data (build_shader_record_
         * compiled).  The FS varying count is the compiled FS's input-slot count.
         * A uniform slot outside the handled subset declines the whole draw
         * (expand_uniforms returns -1) so the QPU is never fed a wrong value. */
        const v3d_3d_prog_t *pr = &_progs[prog_handle];
        /* Rebuild the per-unit V3D71 texture records this FS samples; a bound
         * unit whose pixel data is not 64-byte aligned declines the draw (the
         * caller then rasterizes on the CPU).  Must precede expand_uniforms,
         * which references _texstate_p / _sampler_p for the TMU_CONFIG slots. */
        if (build_tex_state_records(ctex) != 0)
            return decline("ctex base not 64B aligned");
        if (expand_uniforms(_cvs_unif_va, pr->vs_unif_contents, pr->vs_unif_data,
                            pr->vs_num_uniforms, unif, num_uniforms, 0u, ctex,
                            fx, fy, 0.5f, 0.5f, NULL, 0u, 0u, 0u) != 0)
            return decline("vs uniforms");
        if (expand_uniforms(_cfs_unif_va, pr->fs_unif_contents, pr->fs_unif_data,
                            pr->fs_num_uniforms, unif, num_uniforms, 0u, ctex,
                            fx, fy, 0.5f, 0.5f, NULL, 0u, 0u, 0u) != 0)
            return decline("fs uniforms");
        if (expand_uniforms(_ccs_unif_va, pr->coord_unif_contents,
                            pr->coord_unif_data, pr->coord_num_uniforms,
                            unif, num_uniforms, 0u, ctex,
                            fx, fy, 0.5f, 0.5f, NULL, 0u, 0u, 0u) != 0)
            return decline("coord uniforms");
        build_shader_record_compiled(pr->fs_qpu_iova, _cfs_unif_p,
                            pr->fs_threads, pr->fs_single_seg,
                            pr->vs_qpu_iova, _cvs_unif_p,
                            pr->vs_threads, pr->vs_single_seg,
                            pr->coord_qpu_iova, _ccs_unif_p,
                            pr->coord_threads, pr->coord_single_seg,
                            pr->fs_num_inputs & 0x1Fu,
                            pr->coord_vpm_output_size, pr->coord_vpm_input_size,
                            pr->vs_vpm_output_size, pr->vs_vpm_input_size);
        /* col_iova != 0 selects the two-attribute mesh path (attr0 = pos,
         * attr1 = normal, both float3 stride 12); col_iova == 0 keeps the
         * pos-only full-screen-quad path (shadertoy). */
        if (col_iova != 0) {
            build_attr_records_compiled2(pos_iova, col_iova);
            sr_nattrs = 2;
        } else {
            build_attr_records_compiled(pos_iova);
            sr_nattrs = 1;
        }
        __asm__ __volatile__("dsb sy" ::: "memory");
        goto submit;
    }

    /* fixed-function paths: [0]=1.0 w, [1]=x scale (w/2), [2]=y scale (-h/2),
     * [3]=0.5 z scale, [4]=0.5 z off. */
    _unif_va[0] = 0x3F800000u;                  /* 1.0 (w) */
    _unif_va[1] = ux;
    _unif_va[2] = uy;
    _unif_va[3] = 0x3F000000u;                  /* 0.5 z scale */
    _unif_va[4] = 0x3F000000u;                  /* 0.5 z offset */

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
     * because the binner only reads pos.  All five hand-written shaders are
     * 1-thread with a single (thread-end) THRSW and no earlier THRSW pair, so
     * they are the single_seg=1 shape: start_in_final_thread_section set. */
    if (tex_path) {
        build_shader_record(_frag_tex_iova, _tex_unif_p, sh_flags(1u, 1u),
                            _vtx_tex_iova, _unif_p, sh_flags(1u, 1u),
                            _coord_iova, _cs_unif_p, sh_flags(1u, 1u), 2u);
        build_attr_records_tex(pos_iova, tex->uv_iova);
    } else {
        build_shader_record(_frag_iova, _unif_p, sh_flags(1u, 1u),
                            _vtx_iova, _unif_p, sh_flags(1u, 1u),
                            _coord_iova, _cs_unif_p, sh_flags(1u, 1u), 4u);
        build_attr_records(pos_iova, col_iova);
    }
    sr_nattrs = 2;
    __asm__ __volatile__("dsb sy" ::: "memory");

submit:
    core = v3d_g2d_core_regs();
    v3d_g2d_op_begin();
    snprintf(_job_desc, sizeof(_job_desc),
             "draw %s prog %d mode %d nverts %u %ux%u tex %d z %d attrs %u",
             compiled ? "compiled" : "fixed", (int)prog_handle, mode, nverts,
             w, h, tex_path, z_iova != 0, sr_nattrs);

    /* bin: the primitive list */
    len = build_draw_bcl(mode, nverts, _ssr_p, sr_nattrs, w, h,
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

/* ------------------------------------------------------------------ */
/* Compiled-program upload (WS4): copy the VS/FS QPU words into GPU dma via   */
/* alloc_stage and cache the IOVAs + prog_data (uniform descriptors/args,     */
/* VPM sizes, attribute/input maps, FS flags) by handle.  No dispatch happens */
/* here; a later draw references the returned handle and expands the uniform  */
/* streams into the shared per-draw dma staging.  A partially-uploaded       */
/* program on a dma failure gives its blocks back and releases the slot.     */
/* ------------------------------------------------------------------ */
int v3d_3d_program(const v3d_3d_prog_desc_t *desc)
{
    v3d_3d_prog_t *pr;
    void *va;
    uint32_t iova, i;
    int slot = -1;
    /* A FULL-SCREEN client program (shadertoy-style) arrives as a PURE-COMPUTE
     * blob: the pgl_g3d gate compiles only the compute wrap and deliberately
     * keeps an empty VS/FS/COORD triple.  The compute route in v3d_3d_draw
     * dispatches through draw_compute() using the cs fields alone, so the
     * raster stages are optional whenever a cs kernel is present. */
    int cs_only = (desc != NULL && desc->cs_qpu_words != 0 &&
                   desc->vs_qpu_words == 0 && desc->fs_qpu_words == 0 &&
                   desc->coord_qpu_words == 0);

    if (!_ready || desc == NULL)
        return V3D_3D_PROG_UNSUPPORTED;

    /* validate the stage payloads against the cache caps (decline, never
     * truncate, so an out-of-range program falls back to the CPU path).
     * Emptiness alone is NOT a cap violation: a pure-compute program is
     * legitimately empty in vs/fs/coord and is checked by cs_only below. */
    if (desc->vs_qpu_words > V3D_3D_PROG_MAX_QPU ||
        desc->fs_qpu_words > V3D_3D_PROG_MAX_QPU ||
        desc->coord_qpu_words > V3D_3D_PROG_MAX_QPU ||
        desc->cs_qpu_words > V3D_3D_PROG_MAX_QPU ||
        desc->vs_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
        desc->fs_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
        desc->coord_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
        desc->cs_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
        desc->fs_num_inputs > V3D_3D_PROG_MAX_INPUTS ||
        (!cs_only && (desc->vs_qpu_words == 0 || desc->fs_qpu_words == 0 ||
                      desc->coord_qpu_words == 0)))
        slog("v3d_3d: program rejected: qpu vs/fs/coord/cs %u/%u/%u/%u (max %u) "
             "unif %u/%u/%u/%u (max %u) inputs %u (max %u)\r\n",
             desc->vs_qpu_words, desc->fs_qpu_words, desc->coord_qpu_words,
             desc->cs_qpu_words, V3D_3D_PROG_MAX_QPU,
             desc->vs_num_uniforms, desc->fs_num_uniforms,
             desc->coord_num_uniforms, desc->cs_num_uniforms,
             V3D_3D_PROG_MAX_UNIF, desc->fs_num_inputs, V3D_3D_PROG_MAX_INPUTS);
    /* an all-empty program (no raster stages, no compute) can never draw */
    if (desc->vs_qpu_words == 0 && desc->fs_qpu_words == 0 &&
        desc->coord_qpu_words == 0 && desc->cs_qpu_words == 0)
        return V3D_3D_PROG_UNSUPPORTED;
    if (!cs_only) {
        if (desc->vs_qpu == NULL || desc->vs_qpu_words == 0 ||
            desc->vs_qpu_words > V3D_3D_PROG_MAX_QPU)
            return V3D_3D_PROG_UNSUPPORTED;
        if (desc->fs_qpu == NULL || desc->fs_qpu_words == 0 ||
            desc->fs_qpu_words > V3D_3D_PROG_MAX_QPU)
            return V3D_3D_PROG_UNSUPPORTED;
        /* the coordinate (binning) shader is mandatory on the compiled raster
         * path: WS1 always emits it, so a blob without one is malformed/stale
         * (the version is already checked upstream) and is declined rather
         * than binned wrong.  Pure-compute programs never bin. */
        if (desc->coord_qpu == NULL || desc->coord_qpu_words == 0 ||
            desc->coord_qpu_words > V3D_3D_PROG_MAX_QPU)
            return V3D_3D_PROG_UNSUPPORTED;
    }
    if (desc->vs_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
        desc->fs_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
        desc->coord_num_uniforms > V3D_3D_PROG_MAX_UNIF)
        return V3D_3D_PROG_UNSUPPORTED;
    if (desc->fs_num_inputs > V3D_3D_PROG_MAX_INPUTS)
        return V3D_3D_PROG_UNSUPPORTED;
    /* a non-empty uniform stream must carry both its descriptor and values */
    if (desc->vs_num_uniforms &&
        (desc->vs_unif_contents == NULL || desc->vs_unif_data == NULL))
        return V3D_3D_PROG_UNSUPPORTED;
    if (desc->fs_num_uniforms &&
        (desc->fs_unif_contents == NULL || desc->fs_unif_data == NULL))
        return V3D_3D_PROG_UNSUPPORTED;
    if (desc->coord_num_uniforms &&
        (desc->coord_unif_contents == NULL || desc->coord_unif_data == NULL))
        return V3D_3D_PROG_UNSUPPORTED;
    /* optional compute stage */
    if (desc->cs_qpu_words != 0 &&
        (desc->cs_qpu == NULL || desc->cs_qpu_words > V3D_3D_PROG_MAX_QPU ||
         desc->cs_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
         (desc->cs_num_uniforms &&
          (desc->cs_unif_contents == NULL || desc->cs_unif_data == NULL))))
        return V3D_3D_PROG_UNSUPPORTED;
    /* optional second compute stage (pass-1 vertex transform) */
    if (desc->cs2_qpu_words != 0 &&
        (desc->cs2_qpu == NULL || desc->cs2_qpu_words > V3D_3D_PROG_MAX_QPU ||
         desc->cs2_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
         (desc->cs2_num_uniforms &&
          (desc->cs2_unif_contents == NULL || desc->cs2_unif_data == NULL))))
        return V3D_3D_PROG_UNSUPPORTED;
    /* a two-pass geom program must carry BOTH stages; a geom raster kernel
     * without its xform pass (or vice versa) cannot run, so decline it. */
    if (desc->cs_is_geom && (desc->cs_qpu_words == 0 || desc->cs2_qpu_words == 0))
        return V3D_3D_PROG_UNSUPPORTED;
    /* cs_geom_tpw invariant: the one-lane-per-triangle raster kernels cover
     * exactly one triangle per lane, so a nonzero tpw must equal the cs
     * workgroup lane count - anything else would silently mis-cover the
     * mesh (a too-wide grid drops triangles, a too-narrow one wastes the
     * guard exits).  0 keeps the legacy one-workgroup-per-triangle grid. */
    if (desc->cs_geom_tpw != 0u &&
        desc->cs_geom_tpw != desc->cs_local_size[0] * desc->cs_local_size[1] *
                             desc->cs_local_size[2])
        return V3D_3D_PROG_UNSUPPORTED;

    /* reserve a free cache slot */
    for (i = 0; i < V3D_3D_MAX_PROGS; i++) {
        if (!_progs[i].used) { slot = (int)i; break; }
    }
    if (slot < 0)
        return V3D_3D_PROG_UNSUPPORTED;      /* cache full */
    pr = &_progs[slot];
    memset(pr, 0, sizeof(*pr));

    /* upload the raster-stage QPU code into GPU dma (page-aligned -> 8-byte
     * aligned, satisfying the shader record's address field).  A pure-compute
     * program has no raster stages; its slot fields stay zeroed and the
     * compute route never reads them. */
    if (!cs_only) {
        if (alloc_stage_raw(desc->vs_qpu_words * 4u, &va, &iova, &pr->vs_qpu_raw) != 0) {
            slog("v3d_3d: program vs alloc_stage %u failed\r\n", desc->vs_qpu_words * 4u);
            goto fail;
        }
        memcpy(va, desc->vs_qpu, desc->vs_qpu_words * 4u);
        pr->vs_qpu_iova  = iova;
        pr->vs_qpu_words = desc->vs_qpu_words;
        publish_stage(iova, desc->vs_qpu_words * 4u);

        /* upload the FS QPU code */
        if (alloc_stage_raw(desc->fs_qpu_words * 4u, &va, &iova, &pr->fs_qpu_raw) != 0)
            goto fail;
        memcpy(va, desc->fs_qpu, desc->fs_qpu_words * 4u);
        pr->fs_qpu_iova  = iova;
        pr->fs_qpu_words = desc->fs_qpu_words;
        publish_stage(iova, desc->fs_qpu_words * 4u);

        /* upload the COORD (binning) QPU code */
        if (alloc_stage_raw(desc->coord_qpu_words * 4u, &va, &iova, &pr->coord_qpu_raw) != 0)
            goto fail;
        memcpy(va, desc->coord_qpu, desc->coord_qpu_words * 4u);
        pr->coord_qpu_iova  = iova;
        pr->coord_qpu_words = desc->coord_qpu_words;
        publish_stage(iova, desc->coord_qpu_words * 4u);
    }

    /* upload the optional COMPUTE QPU code */
    if (desc->cs_qpu_words != 0) {
        if (alloc_stage_raw(desc->cs_qpu_words * 4u, &va, &iova, &pr->cs_qpu_raw) != 0)
            goto fail;
        memcpy(va, desc->cs_qpu, desc->cs_qpu_words * 4u);
        pr->cs_qpu_iova  = iova;
        pr->cs_qpu_words = desc->cs_qpu_words;
        publish_stage(iova, desc->cs_qpu_words * 4u);
        if (desc->cs_num_uniforms) {
            memcpy(pr->cs_unif_contents, desc->cs_unif_contents,
                   desc->cs_num_uniforms * 4u);
            memcpy(pr->cs_unif_data, desc->cs_unif_data,
                   desc->cs_num_uniforms * 4u);
        }
        pr->cs_num_uniforms = desc->cs_num_uniforms;
        pr->cs_threads      = desc->cs_threads;
        pr->cs_single_seg   = desc->cs_single_seg;
        pr->cs_local_size[0] = desc->cs_local_size[0];
        pr->cs_local_size[1] = desc->cs_local_size[1];
        pr->cs_local_size[2] = desc->cs_local_size[2];
        pr->cs_shared_size  = desc->cs_shared_size;
        pr->cs_spill_size   = desc->cs_spill_size;
        pr->cs_is_geom      = desc->cs_is_geom;
        pr->cs_geom_tpw     = desc->cs_geom_tpw;
        /* Register-spill scratch: mesa's allocator spilled this kernel (it emits
         * QUNIFORM_SPILL_OFFSET / _SIZE_PER_THREAD), addressing a thread's slot as
         * TIDX*SPILL_SIZE_PER_THREAD + (EIDX<<2) + SPILL_OFFSET where TIDX =
         * (core<<6)|(qpu<<2)|thread.  Size the BO exactly as mesa does -
         * TIDX packs as (core<<6)|(qpu<<2)|thread, so every slice reserves a
         * 64-wide TIDX block no matter how many of its QPUs run; the scratch
         * must therefore be slices*64*spill_size, not qpus*4*spill_size, or the
         * non-zero cores' threads spill out of bounds (simple non-spilling
         * shader regions still render fine while spilled ones tear).  Allocate
         * it once here as DMA (MMU-mapped + phy-validated by alloc_stage_raw).
         * The kernel reaches it through TMU-general (L2T) ops, the same path as
         * the SSBO framebuffer, so the per-dispatch V3D_G2D_MAINT_ALL full-range
         * L2 clean+invalidate drops any stale scratch lines and the
         * GPU-internal buffer needs no writeback.  Without this the draw
         * declines (unif type 32) to the CPU. */
        if (desc->cs_spill_size != 0) {
            uint32_t scratch = (uint32_t)v3d_g2d_num_slices() * 64u *
                               desc->cs_spill_size;
            if (alloc_stage_raw(scratch, &va, &iova, &pr->cs_spill_raw) != 0) {
                slog("v3d_3d: cs spill scratch %u bytes alloc failed\r\n", scratch);
                goto fail;
            }
            pr->cs_spill_iova = iova;
        }
    }

    /* upload the optional second COMPUTE QPU (pass-1 vertex transform) */
    if (desc->cs2_qpu_words != 0) {
        if (alloc_stage_raw(desc->cs2_qpu_words * 4u, &va, &iova, &pr->cs2_qpu_raw) != 0)
            goto fail;
        memcpy(va, desc->cs2_qpu, desc->cs2_qpu_words * 4u);
        pr->cs2_qpu_iova  = iova;
        pr->cs2_qpu_words = desc->cs2_qpu_words;
        publish_stage(iova, desc->cs2_qpu_words * 4u);
        if (desc->cs2_num_uniforms) {
            memcpy(pr->cs2_unif_contents, desc->cs2_unif_contents,
                   desc->cs2_num_uniforms * 4u);
            memcpy(pr->cs2_unif_data, desc->cs2_unif_data,
                   desc->cs2_num_uniforms * 4u);
        }
        pr->cs2_num_uniforms = desc->cs2_num_uniforms;
        pr->cs2_threads      = desc->cs2_threads;
        pr->cs2_single_seg   = desc->cs2_single_seg;
        pr->cs2_local_size[0] = desc->cs2_local_size[0];
        pr->cs2_local_size[1] = desc->cs2_local_size[1];
        pr->cs2_local_size[2] = desc->cs2_local_size[2];
        pr->cs2_shared_size  = desc->cs2_shared_size;
        pr->cs2_uniform_blob_dwords = desc->cs2_uniform_blob_dwords;
    }

    /* cache the uniform layout descriptors AND their per-slot args on the CPU.
     * The GPU-resident uniform buffer is the shared per-draw expanded staging
     * (_cvs_unif / _cfs_unif) that the QUNIFORM interpreter fills at draw time
     * from these descriptors + args + the per-draw GL uniform blob, so the raw
     * arg stream is deliberately NOT uploaded here (vs_unif_iova/fs_unif_iova
     * stay 0). */
    if (desc->vs_num_uniforms) {
        memcpy(pr->vs_unif_contents, desc->vs_unif_contents,
               desc->vs_num_uniforms * 4u);
        memcpy(pr->vs_unif_data, desc->vs_unif_data,
               desc->vs_num_uniforms * 4u);
    }
    pr->vs_num_uniforms = desc->vs_num_uniforms;

    if (desc->fs_num_uniforms) {
        memcpy(pr->fs_unif_contents, desc->fs_unif_contents,
               desc->fs_num_uniforms * 4u);
        memcpy(pr->fs_unif_data, desc->fs_unif_data,
               desc->fs_num_uniforms * 4u);
    }
    pr->fs_num_uniforms = desc->fs_num_uniforms;

    if (desc->coord_num_uniforms) {
        memcpy(pr->coord_unif_contents, desc->coord_unif_contents,
               desc->coord_num_uniforms * 4u);
        memcpy(pr->coord_unif_data, desc->coord_unif_data,
               desc->coord_num_uniforms * 4u);
    }
    pr->coord_num_uniforms = desc->coord_num_uniforms;

    /* cache the vertex-stage prog_data */
    pr->vs_threads           = desc->vs_threads;
    pr->vs_single_seg        = desc->vs_single_seg;
    pr->vs_vpm_input_size    = desc->vs_vpm_input_size;
    pr->vs_vpm_output_size   = desc->vs_vpm_output_size;
    pr->vs_vcm_cache_size    = desc->vs_vcm_cache_size;
    pr->vs_separate_segments = desc->vs_separate_segments;
    memcpy(pr->vs_vattr_sizes, desc->vs_vattr_sizes,
           sizeof(pr->vs_vattr_sizes));
    memcpy(pr->vs_driver_location_map, desc->vs_driver_location_map,
           sizeof(pr->vs_driver_location_map));

    /* cache the fragment-stage prog_data */
    pr->fs_threads       = desc->fs_threads;
    pr->fs_single_seg    = desc->fs_single_seg;
    pr->fs_needs_coords  = desc->fs_needs_coords;
    pr->fs_uses_center_w = desc->fs_uses_center_w;
    pr->fs_writes_z      = desc->fs_writes_z;
    pr->fs_disable_ez    = desc->fs_disable_ez;
    pr->fs_num_inputs    = desc->fs_num_inputs;
    memcpy(pr->fs_input_slots, desc->fs_input_slots,
           sizeof(pr->fs_input_slots));

    /* cache the coordinate-stage prog_data (coord VPM segments for the record) */
    pr->coord_threads           = desc->coord_threads;
    pr->coord_single_seg        = desc->coord_single_seg;
    pr->coord_vpm_input_size    = desc->coord_vpm_input_size;
    pr->coord_vpm_output_size   = desc->coord_vpm_output_size;
    pr->coord_vcm_cache_size    = desc->coord_vcm_cache_size;
    pr->coord_separate_segments = desc->coord_separate_segments;

    __asm__ __volatile__("dsb sy" ::: "memory");
    pr->used = 1;
    return slot;

fail:
    /* release the slot and the dma blocks already uploaded for it */
    slog("v3d_3d: program %d alloc_stage failed (fs %u coord %u cs %u words)\r\n",
         slot, desc->fs_qpu_words, desc->coord_qpu_words, desc->cs_qpu_words);
    release_prog(pr);
    return -31;
}

/* ------------------------------------------------------------------ */
/* Drop a cached program: give its QPU code blocks back to dma and free   */
/* the slot.  The daemon calls this for programs whose owning client has  */
/* exited (it never tells us), so the 16-slot cache survives repeated app */
/* launches.  A later draw naming the handle is declined (used == 0).     */
/* ------------------------------------------------------------------ */
int v3d_3d_program_free(int prog_handle)
{
    v3d_3d_prog_t *pr;

    if (!_ready || prog_handle < 0 || (uint32_t)prog_handle >= V3D_3D_MAX_PROGS)
        return V3D_3D_PROG_UNSUPPORTED;
    pr = &_progs[prog_handle];
    if (!pr->used)
        return V3D_3D_PROG_UNSUPPORTED;
    pr->used = 0;
    __asm__ __volatile__("dsb sy" ::: "memory");
    release_prog(pr);
    return 0;
}

/* OpenCL compute dispatch (the vc_cl route): the generalisation of
 * draw_compute above for an arbitrary CL kernel.  The program was uploaded as
 * a pure-compute (cs-only) blob through v3d_3d_program, so its cs QPU, uniform
 * layout, threads / single_seg and spill scratch are already cached in the
 * handle's slot.  Where draw_compute hardcodes SSBO binding 0 = the frame
 * buffer and derives the grid from the frame size, here the caller supplies the
 * workgroup grid and the table of __global buffer SSBO IOVAs (binding order),
 * which expand_uniforms resolves for every QUNIFORM_SSBO_OFFSET slot.  The
 * expanded-uniform / job-uniform dma staging, the QUNIFORM interpreter, the op
 * bracket and the CSD submission + L2 maintenance are reused verbatim, so a CL
 * launch is the proven compute path with a caller-owned SSBO table. */
int v3d_3d_cl_dispatch(int32_t prog_handle,
                       const uint32_t wg_count[3], uint32_t wg_size,
                       const uint32_t *ssbo_iova, uint32_t num_ssbo,
                       const uint32_t *job_unif, uint32_t num_job_unif,
                       uint32_t dst_phy, uint32_t dst_span)
{
    v3d_3d_prog_t *pr;
    v3d_g2d_csd_job_t job;
    int rc;

    if (!_ready || prog_handle < 0 ||
        (uint32_t)prog_handle >= V3D_3D_MAX_PROGS)
        return V3D_3D_UNSUPPORTED;
    pr = &_progs[prog_handle];
    if (!pr->used || pr->cs_qpu_words == 0)
        return V3D_3D_UNSUPPORTED;
    /* the CSD has no shared-memory / barrier support in this route, so a
     * kernel that asked for __local storage is declined to the CPU device. */
    if (pr->cs_shared_size != 0)
        return decline("cl: shared memory unsupported");
    if (wg_size == 0 || wg_size > 256u)
        return decline("cl: wg_size");
    if (wg_count == NULL || wg_count[0] == 0)
        return decline("cl: wg_count");

    v3d_g2d_op_begin();
    if (num_job_unif > V3D_3D_JOB_MAX_UNIF ||
        (num_job_unif != 0 && job_unif == NULL)) {
        v3d_g2d_op_end();
        return decline("cl: uniform blob");
    }
    if (num_job_unif != 0)
        memcpy(_job_unif_va, job_unif, num_job_unif * 4u);
    /* expand the kernel's uniform stream: QUNIFORM_SSBO_OFFSET slots index the
     * caller's ssbo_iova[] by binding, QUNIFORM_UNIFORM / UBO_ADDR slots read
     * the packed scalar-arg blob, spill slots resolve from the cached scratch.
     * No sampler state on the CL route (ctex NULL). */
    if (expand_uniforms(_ccu_unif_va, pr->cs_unif_contents, pr->cs_unif_data,
                        pr->cs_num_uniforms, job_unif, num_job_unif,
                        _job_unif_p, NULL, 0.0f, 0.0f, 0.0f, 0.0f,
                        ssbo_iova, num_ssbo,
                        pr->cs_spill_iova, pr->cs_spill_size) != 0) {
        v3d_g2d_op_end();
        return decline("cl: uniforms");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");

    memset(&job, 0, sizeof(job));
    job.code_iova   = pr->cs_qpu_iova;
    job.unif_iova   = _ccu_unif_p;
    job.wg_count[0] = wg_count[0];
    job.wg_count[1] = wg_count[1];
    job.wg_count[2] = wg_count[2];
    job.wg_size     = wg_size;
    job.threads     = pr->cs_threads;
    job.single_seg  = pr->cs_single_seg;
    /* POST writeback covers the hull of every argument buffer the kernel may
     * write (dst_phy/dst_span, computed by vc_cl); 0 selects the full walk. */
    job.dst_phy     = dst_phy;
    job.dst_span    = dst_span;
    job.unif_span   = pr->cs_num_uniforms * 4u;
    job.src_phy     = _job_unif_p;       /* ARM-written scalar-arg blob */
    job.src_span    = num_job_unif * 4u;
    job.tag         = V3D_G2D_TAG_COMPUTE;
    job.budget_ms   = V3D_3D_COMPUTE_BUDGET_MS;
    rc = v3d_g2d_run_csd(&job, V3D_G2D_MAINT_ALL | V3D_G2D_MAINT_ARGS |
                               V3D_G2D_POLL_YIELD | V3D_G2D_POLL_IRQ |
                               V3D_G2D_FAILURE_LOCAL);
    v3d_g2d_op_end();
    if (rc != 0) {
        slog("v3d_3d: cl dispatch rc=%d grid %ux%ux%u wg=%u thr=%u unif=%u ssbo=%u\r\n",
             rc, job.wg_count[0], job.wg_count[1], job.wg_count[2], job.wg_size,
             (uint32_t)pr->cs_threads, pr->cs_num_uniforms, num_ssbo);
        return rc > 0 ? V3D_3D_TIMEOUT : V3D_3D_MMU_FAULT;
    }
    return V3D_3D_DONE;
}
