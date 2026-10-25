/*
 * adreno_prog.h - Q8B (Adreno 690) a6xx compute-shader program blob contract.
 *
 * WHY THIS EXISTS (and why it is NOT g2dclient.h's g2d_pgl_program_blob_t):
 *   The shared portablegl offload protocol ships a g2d_pgl_program_blob_t whose
 *   payload is VideoCore V3D 7.1 QPU instruction words - the ISA raspi5's V3D
 *   back end consumes.  Those words are meaningless to an Adreno 690, which
 *   runs a6xx ir3 ISA.  Q8B therefore needs its own program blob: a flat
 *   serialization of exactly the fields of Mesa's `struct ir3_shader_variant`
 *   (freedreno/ir3/ir3_shader.h) that the a6xx compute dispatch reads, produced
 *   APP-SIDE by the machine GPU compiler backend
 *   (machines/q8b/system/libs/adrenocompiler: the pglcompiler.h contract
 *   implemented over the vendored mesa freedreno/ir3 tree, GLSL->NIR->ir3->
 *   a6xx; Adreno-specific code lives under the q8b machine, not in the shared
 *   projects/opengl/libs/gles) and consumed DAEMON-SIDE by adreno_3d.c.
 *
 *   The per-kernel field names map 1:1 onto ir3_shader_variant so the app
 *   compiler (adrenocompiler/adreno_backend.c) is a straight dump, not a
 *   re-derivation:
 *     binary[]            <- v->bin, v->info.size bytes
 *     instrlen            <- v->instrlen            (SP_CS_INSTR_SIZE)
 *     constlen            <- v->constlen            (SP_CS_CONST_CONFIG.CONSTLEN = constlen>>2)
 *     shared_size         <- v->shared_size         (SP_CS_CNTL_1.SHARED_SIZE)
 *     local_size[3]       <- v->local_size          (SP_CS_NDRANGE_0 + the grid)
 *     max_reg/max_half_reg<- v->info.max_reg/max_half_reg (SP_CS_CNTL_0 footprints, +1)
 *     double_threadsize   <- v->info.double_threadsize    (SP_CS_CNTL_0.THREADSIZE)
 *     mergedregs          <- v->mergedregs          (SP_CS_CNTL_0.MERGEDREGS)
 *     branchstack_hw      <- ir3_shader_branchstack_hw(v)  (SP_CS_CNTL_0.BRANCHSTACK)
 *     early_preamble      <- v->early_preamble      (SP_CS_CNTL_0.EARLYPREAMBLE)
 *     round_robin_mode    <- v->cs.round_robin_mode (SP_CS_CNTL_0.COMPUTERRMODEEN)
 *     pvtmem_size         <- v->pvtmem_size         (bytes/fiber; 0 => no pvtmem BO)
 *     pvtmem_per_wave     <- v->pvtmem_per_wave     (SP_CS_PVT_MEM_SIZE.PERWAVEMEMLAYOUT)
 *     imm_const_*         <- v->imm_state / const_state->allocs.max_const_offset_vec4
 *     constant_data_*     <- v->constant_data_size / v->info.constant_data_offset
 *     consts_ubo_idx      <- ir3_const_state(v)->consts_ubo.idx (-1 = none)
 *     num_ubos            <- ir3_const_state(v)->num_ubos
 *     ubo_range[]         <- ir3_const_state(v)->ubo_state.range[] (block 0 only)
 *     driver_params_*     <- const_state->allocs.consts[IR3_CONST_ALLOC_DRIVER_PARAMS]
 *     local_invocation_id_regid / work_group_id_regid
 *                           <- ir3_find_sysval_regid(v, SYSTEM_VALUE_*)
 *     ssbo_binding[]      <- the declared layout(binding) of UAV slot i
 *
 * VERSION 2 LAYOUT: a fixed adreno_prog_blob_t header carrying up to TWO
 *   kernels (kern[0] = the main compute kernel: fullscreen effect or the
 *   geometry RASTER pass; kern[1] = the two-pass geometry rasterizer's vertex
 *   TRANSFORM pass; absent <=> binary_words == 0, with the other fields at
 *   their "none" values: counts 0, driver_params_vec4_off =
 *   ADRENO_PROG_NO_DRIVER_PARAMS, regids = ADRENO_PROG_REGID_NONE), followed
 *   immediately by the u32
 *   payload: for kern[0] then kern[1], each kernel's three variable sections
 *   in this order (each present only when its count is non-zero):
 *   binary[binary_words], imm_consts[imm_const_count],
 *   constant_data[constant_data_size/4].  All fields little-endian / naturally
 *   aligned; the header size is a multiple of 4 so the payload is u32-aligned.
 *   payload_bytes is the total byte length of all sections so the receiver
 *   validates the buffer size without walking the counts.
 *
 * PER-DRAW UNIFORMS (the "default uniform block"): the app compiler lowers
 *   every default-block uniform to a load from UBO 0 whose contents are the
 *   per-draw uniform blob portablegl packs by name (pglc_uniform_binding
 *   reflection; uniform_blob_dwords dwords).  ir3 then decides, per range, to
 *   either PUSH the bytes into the constant file (ir3_nir_analyze_ubo_ranges:
 *   ubo_range[] below, src_byte -> dst_vec4 for size_bytes) or keep them as a
 *   descriptor load (ldc) from UBO 0.  The daemon must therefore do BOTH:
 *   CP_LOAD_STATE6 (ST6_CONSTANTS, SS6_DIRECT) each ubo_range from the uniform
 *   BO, and, when num_ubos > 0, point UBO descriptor 0 (ST6_UBO) at the whole
 *   uniform BO so the ldc fallback reads the same bytes.  For the geometry
 *   two-pass program the per-draw blob is the xform region followed by the
 *   raster region; cs2_uniform_blob_dwords is the split point.
 *
 * DRIVER PARAMS: when the kernel reads gl_NumWorkGroups / gl_WorkGroupSize /
 *   base ids, ir3 reserves driver_params_dwords dwords of struct
 *   ir3_driver_params_cs at vec4 offset driver_params_vec4_off and the daemon
 *   fills them per dispatch; ADRENO_PROG_NO_DRIVER_PARAMS means none.
 *
 * SSBO / UAV CONTRACT: the compiler lowers each storage buffer (non-bindless
 *   ir3_ssbo_to_ibo) to UAV slot i == the LINKED block index i, whose
 *   a6xx_tex_const descriptor (16 dwords, fdl6_buffer_view_init R32_UINT
 *   buffer view) the daemon writes at dword 16*i of the per-draw CS UAV
 *   descriptor BO (CP_LOAD_STATE6_FRAG ST6_UAV SS6_INDIRECT SB6_CS_SHADER +
 *   SP_CS_UAV_BASE, SP_CS_CONFIG.NUAV = num_ssbo).  ssbo_binding[i] is the
 *   declared layout(binding) of slot i, which is how the g2d draw request
 *   names the buffers (binding 0 = render target by convention, then the
 *   geometry rasterizer's pos/nrm/depth/tex/... buffers in request order).
 *
 * Q8B hardware tests on 2026-10-22 verified fullscreen compute SSBO writes,
 * pushed per-draw uniforms, strided bounds and A2D/CS transitions through
 * 720x720. This does not validate every field or shader feature (QEMU models
 * no Adreno). The contract follows the ir3_shader_variant fields consumed by
 * tu6_emit_cs_config / tu6_emit_xs / tu6_emit_xs_constants. See
 * test/HANDOVER.md for exact tested artifacts and remaining gaps.
 */
#ifndef ADRENO_PROG_H
#define ADRENO_PROG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 'A','N','D','3' little-endian - deliberately distinct from the V3D blob's
   G2D_PGL_PROGRAM_MAGIC ('G','P','D','3') so a daemon never mistakes one for
   the other. */
#define ADRENO_PROG_MAGIC     0x33444e41u
/* VERSION 4 adds the sampled-texture metadata (num_samp / tex_const_off /
 * tex_type[]) to adreno_prog_kernel_t, growing the fixed blob header.  The
 * app-side compiler (adrenocompiler) and the daemon (adreno) are always
 * rebuilt together; the bumped version makes a half-updated pair reject the
 * blob at the magic/version gate instead of misreading the new layout. */
#define ADRENO_PROG_VERSION   4u
/* Same wire layout; raster grid covers pixels, each owning its colour/depth.
 * A separate version makes old daemons reject the different dispatch shape. */
#define ADRENO_PROG_PIXEL_VERSION 5u

/* Maximum storage-buffer bindings a compiled kernel may declare.  Sized for the
   portablegl fullscreen-compute set and the geometry rasterizer (render target
   + a handful of input buffers) with headroom; mirrors the small fixed
   descriptor count the daemon caches per program. */
#define ADRENO_PROG_MAX_SSBO        8u

/* Maximum sampled-texture units a compiled kernel may read.  Matches the
   client's G2D_PGL_MAX_TEX_UNITS (4): the engine binds iChannel0..3 to ctex
   units 0..3, and sampler slot i is fed by ctex unit i. */
#define ADRENO_PROG_MAX_TEX         4u
/* tex_type[] values: a plain 2D image vs a 6-face cubemap. */
#define ADRENO_PROG_TEX_2D          0u
#define ADRENO_PROG_TEX_CUBE        1u

/* Maximum pushed UBO-0 ranges (IR3_MAX_UBO_PUSH_RANGES is 32 upstream; the
   per-draw uniform blobs here are a few dozen dwords, so 8 is ample and the
   compiler fails the compile rather than truncating). */
#define ADRENO_PROG_MAX_UBO_RANGES  8u

/* ir3's regid(63,0): "this system value is not read by the kernel". */
#define ADRENO_PROG_REGID_NONE      0xfcu

/* driver_params_vec4_off value meaning "kernel reads no driver params". */
#define ADRENO_PROG_NO_DRIVER_PARAMS 0xffffffffu

/* One pushed range of the per-draw uniform blob (UBO 0): copy size_bytes
   starting at byte src_byte of the blob into the constant file at vec4
   dst_vec4 (CP_LOAD_STATE6 ST6_CONSTANTS DST_OFF = dst_vec4,
   NUM_UNIT = size_bytes / 16). */
typedef struct {
	uint32_t src_byte;
	uint32_t dst_vec4;
	uint32_t size_bytes;         /* multiple of 16                          */
} adreno_prog_ubo_range_t;

/* One compiled a6xx compute kernel (one ir3_shader_variant). */
typedef struct {
	/* --- assembled a6xx binary (v->bin) --- */
	uint32_t binary_words;       /* u32 count of the assembled ir3 binary   */
	uint32_t instrlen;           /* v->instrlen -> SP_CS_INSTR_SIZE         */

	/* --- constant / shared / grid geometry --- */
	uint32_t constlen;           /* v->constlen (vec4 units)                */
	uint32_t shared_size;        /* v->shared_size (bytes)                  */
	uint32_t local_size[3];      /* v->local_size (workgroup x/y/z)         */

	/* --- SP_CS_CNTL_0 inputs --- */
	uint8_t  max_reg;            /* v->info.max_reg   (footprint = +1)      */
	uint8_t  max_half_reg;       /* v->info.max_half_reg (footprint = +1)   */
	uint8_t  double_threadsize;  /* v->info.double_threadsize               */
	uint8_t  mergedregs;         /* v->mergedregs                           */
	uint32_t branchstack_hw;     /* ir3_shader_branchstack_hw(v)            */
	uint8_t  early_preamble;     /* v->early_preamble                       */
	uint8_t  round_robin_mode;   /* v->cs.round_robin_mode                  */
	uint8_t  reserved0[2];

	/* --- private (spill/stack) memory --- */
	uint32_t pvtmem_size;        /* v->pvtmem_size bytes/fiber (0 => none)  */
	uint8_t  pvtmem_per_wave;    /* v->pvtmem_per_wave                      */
	uint8_t  reserved1[3];

	/* --- immediate constants (v->imm_state) --- */
	uint32_t imm_const_count;    /* v->imm_state.count (u32 values)         */
	uint32_t imm_const_base;     /* const_state->allocs.max_const_offset_vec4*/

	/* --- immutable constant data (consts_ubo) --- */
	uint32_t constant_data_size; /* v->constant_data_size (bytes)           */
	uint32_t constant_data_off;  /* v->info.constant_data_offset in the bin BO*/
	int32_t  consts_ubo_idx;     /* ir3_const_state(v)->consts_ubo.idx (-1) */

	/* --- UBO descriptors + pushed uniform ranges (see header comment) --- */
	uint32_t num_ubos;           /* UBO descriptors the kernel still ldc's   */
	uint32_t num_ubo_ranges;     /* active entries in ubo_range[]           */
	adreno_prog_ubo_range_t ubo_range[ADRENO_PROG_MAX_UBO_RANGES];

	/* --- driver params (struct ir3_driver_params_cs) --- */
	uint32_t driver_params_vec4_off; /* ADRENO_PROG_NO_DRIVER_PARAMS = none */
	uint32_t driver_params_dwords;   /* dwords the daemon fills (<= 16)     */

	/* --- system-value reg ids (SP_CS_CONST_CONFIG_0) --- */
	uint32_t local_invocation_id_regid; /* ir3_find_sysval_regid(LOCAL_INVOCATION_ID) */
	uint32_t work_group_id_regid;       /* ir3_find_sysval_regid(WORKGROUP_ID)        */

	/* --- SSBO -> UAV slot bindings (see the contract comment above) --- */
	uint32_t num_ssbo;           /* UAV slots == active entries below       */
	uint32_t ssbo_binding[ADRENO_PROG_MAX_SSBO];  /* declared binding of slot i */

	/* --- per-draw uniform blob span (dwords) this kernel reads as UBO 0 --- */
	uint32_t uniform_blob_dwords;

	/* --- sampled textures / samplers (VERSION 4).  num_samp 1:1 texture +
	   sampler pairs the kernel reads with a bindful `sam` (0 => purely
	   procedural, the daemon emits no texture state).  Following the a6xx
	   bindful model (mesa fd6_emit_textures), the daemon builds one 16-dword
	   A6XX_TEX_CONST per slot into a dedicated texture-descriptor BO pointed
	   to by SP_CS_TEXMEMOBJ_BASE, and one 4-dword A6XX_TEX_SAMP per slot into
	   a sampler-descriptor BO pointed to by SP_CS_SAMPLER_BASE; the counts go
	   in SP_CS_CONFIG.NTEX/.NSAMP.  The descriptors do NOT live in the const
	   file, so CONSTLEN is unchanged.  tex_type[i] is ADRENO_PROG_TEX_2D /
	   _CUBE.  Slot i is fed by ctex unit i.  tex_const_off is reserved (0). --- */
	uint32_t num_samp;
	uint32_t tex_const_off;      /* reserved, always 0 (see comment above)   */
	uint8_t  tex_type[ADRENO_PROG_MAX_TEX];
	uint8_t  reserved_tex[3];
} adreno_prog_kernel_t;

typedef struct {
	uint32_t magic;              /* ADRENO_PROG_MAGIC                       */
	uint32_t version;            /* ADRENO_PROG_VERSION                     */

	/* geometry-rasterizer dispatch kind (pglc_blob_opts.geom_kind: 0 = plain
	   fullscreen program, 1 = gouraud, 2 = textured, 3 = instanced,
	   4 = colour-modulated textured) and the raster pass's triangles per
	   workgroup (0 = one workgroup per triangle). */
	uint8_t  is_geom;
	uint8_t  reserved;
	uint16_t geom_tpw;

	/* split point of the concatenated per-draw uniform blob: the first
	   cs2_uniform_blob_dwords dwords feed kern[1] (xform), the rest kern[0]. */
	uint32_t cs2_uniform_blob_dwords;

	adreno_prog_kernel_t kern[2]; /* [0] main / raster CS, [1] xform CS or 0 */

	uint32_t payload_bytes;      /* total bytes of all payload sections     */
	/* variable payload follows immediately after this header, for kern[0]
	   then kern[1]:
	     binary[binary_words]
	     imm_consts[imm_const_count]
	     constant_data[constant_data_size / 4]                              */
} adreno_prog_blob_t;

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_PROG_H */
