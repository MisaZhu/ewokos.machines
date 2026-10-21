/*
 * adreno_3d.c - Adreno 690 (SC8280XP / Q8B) a6xx compute-shader (CS) 3D back end.
 *
 * Implements the daemon side of the portablegl 3D offload for Q8B: it loads a
 * precompiled a6xx compute program (the adreno_prog.h VERSION 2 blob the APP
 * side produces with the machine GPU compiler backend,
 * machines/q8b/system/libs/adrenocompiler over the vendored Mesa ir3) and
 * dispatches it through CP_EXEC_CS.  Two program shapes exist:
 *
 *   is_geom == 0  fullscreen kernel: one invocation per pixel writes the
 *                 framebuffer (shadertoy-style effects).
 *   is_geom != 0  two-pass compute geometry rasterizer, the same kernels and
 *                 SSBO binding tables raspi5's v3d_3d.c draw_compute_geom runs
 *                 on V3D: kern[1] (vertex TRANSFORM, one invocation per vertex)
 *                 writes packed records into a daemon-owned `vout` buffer, then
 *                 kern[0] (RASTER, ceil(ntris/tpw) workgroups) reads vout and
 *                 writes colour + atomicMin depth.
 *
 * This is the sanctioned 3D route for Adreno - a CS COMPUTE dispatch, NOT the
 * binner / fixed-function 3D pipeline - because a compute kernel reproduces
 * portablegl's rasterization without needing a full a6xx graphics-pipeline
 * state (VFD/VPC/RB/GRAS) bring-up that EwokOS has no validated path for.
 *
 * ---------------------------------------------------------------------------
 * PROVENANCE (every offset / bitfield / enum / emit order verified against)
 * ---------------------------------------------------------------------------
 *   Mesa freedreno turnip (main branch), the A6XX CS dispatch:
 *     vulkan/tu_shader.cc
 *       tu6_emit_xs(MESA_SHADER_COMPUTE)     -> SP_CS_CNTL_0 / INSTR_SIZE /
 *                              PROGRAM_COUNTER_OFFSET / BASE(qw) / PVT_MEM_*
 *       tu6_emit_cs_config                   -> SP_UPDATE_CNTL, SP_CS_CNTL_1,
 *                              HLSQ_CS_CTRL_REG1 (has_lpac), SP_CS_CONST_CONFIG_0,
 *                              SP_CS_WGE_CNTL, SP_CS_WIE_CNTL_0/1 (has_lpac)
 *       tu6_emit_xs_constants                -> CP_LOAD_STATE6_FRAG shader preload
 *                              (SS6_INDIRECT), immediates (SS6_DIRECT), consts_ubo
 *       tu6_get_pvtmem_config                -> per_fiber/per_sp/total formulas
 *     vulkan/tu_pipeline.cc
 *       tu6_emit_xs_config / sp_xs_config    -> SP_CS_CONST_CONFIG + SP_CS_CONFIG
 *     vulkan/tu_cmd_buffer.cc
 *       tu_set_render_mode(RM6_COMPUTE)      -> CP_SET_MARKER
 *       A6XX-aligned dispatch                -> SP_CS_NDRANGE_0..6 + CP_EXEC_CS
 *     fdl/fd6_view.cc fdl6_buffer_view_init  -> a6xx_tex_const MEMOBJ SSBO view
 *   Mesa freedreno gallium (the non-bindless model this back end uses):
 *     fd6_emit.cc fd6_emit_cs_state          -> CP_LOAD_STATE6_FRAG(ST6_IBO/UAV,
 *                              SS6_INDIRECT, SB6_CS_SHADER) + SP_CS_IBO(UAV_BASE)
 *                              + SP_CS_IBO_COUNT(USIZE) for the SSBO descriptors
 *     fd6_const.cc ir3_emit_user_consts      -> one ST6_CONSTANTS upload per
 *                              ir3_ubo_range (DST_OFF = range->offset/16,
 *                              size clamped to 16*constlen - offset)
 *     fd6_const.cc fd6_emit_ubos             -> ST6_UBO SS6_DIRECT DST_OFF(0)
 *                              NUM_UNIT(num_ubos), 2 dwords per descriptor
 *     ir3/ir3_shader.h struct ir3_driver_params_cs -> the driver-param layout
 *     common/freedreno_devices.py (A690)     -> num_ccu=8 (=> num_sp_cores=8),
 *                              fibers_per_sp=128*2*16=4096, instr_cache_size=127,
 *                              has_lpac=True, supports_double_threadsize=True
 *     ir3/instr-a3xx.h                       -> regid(63,0)=INVALID_REG=0xfc
 *
 * Register offsets in a6xx_regs.h are the raw PKT4 register indices (NOT the *4
 * MMIO word indices).  A CS dispatch never touches MMIO: the whole operation is
 * a PM4 dword stream the CP fetches from the IB, submitted via adreno_ring_submit.
 *
 * ---------------------------------------------------------------------------
 * HONEST GAPS - NOT hardware-validated on SC8280XP (QEMU models no Adreno, and
 * QEMU is rejected as a validation environment).  Items to confirm on silicon:
 *
 *   1. SSBO DESCRIPTOR APERTURE.  ir3 lowers a non-bindless SSBO access to an
 *      IBO/UAV-slot instruction (ldib/stib/atomic, ir3_ssbo_to_ibo), so this
 *      back end follows gallium fd6: the 16-dword R32_UINT buffer views are
 *      uploaded with CP_LOAD_STATE6_FRAG(ST6_UAV, SS6_INDIRECT, SB6_CS_SHADER)
 *      and SP_CS_UAV_BASE / SP_CS_USIZE, with SP_CS_CONFIG.NUAV = num_ssbo.
 *      turnip instead goes fully bindless.  The slot<->descriptor convention
 *      (UAV slot i == linked block i, binding via ssbo_binding[i]) is the
 *      compiler contract in adreno_prog.h; a mismatch faults or reads garbage.
 *
 *   2. PER-DRAW UNIFORMS.  Default-block uniforms are UBO 0.  ir3 pushes the
 *      ranges it analysed into the constant file (ubo_range[], uploaded here
 *      inline as ST6_CONSTANTS) and may keep others as ldc from descriptor 0,
 *      so UBO descriptor 0 always points at a per-draw GPU copy of the blob.
 *      Both paths are emitted exactly as fd6_const.cc does; whether A690 needs
 *      any additional const-state invalidate between dispatches is untested.
 *
 *   3. DRIVER PARAMS.  struct ir3_driver_params_cs is filled per dispatch
 *      (num_work_groups, local size, subgroup size) when the kernel reserved
 *      it; work_dim is 3 and base_group / workgroup_id are 0 (no indirect).
 *
 *   4. STATIC STATE ASSUMPTION.  turnip emits a large HLSQ/UCHE/TPC context and
 *      magic-register state (fd6 magic_regs, the raw_magic_regs list for A690)
 *      before any dispatch.  This back end emits only the CS registers the
 *      dispatch itself reads, relying on adreno_a6xx.c bring-up for the rest.  If
 *      a CS dispatch needs additional static state on A690 it must be added.
 *
 *   5. pvtmem BO SIZE.  total = num_sp_cores(8) * per_sp_size with
 *      per_sp = align(per_fiber * fibers_per_sp(4096), 4096).  For a non-spilling
 *      kernel pvtmem_size == 0 and NO pvtmem BO is allocated (all PVT_MEM_*
 *      registers written 0), matching tu6_get_pvtmem_config's zero path.
 *
 *   6. TWO-PASS COHERENCY.  The geometry path submits pass 1 and pass 2 as two
 *      ring submissions: adreno_ring_submit's CACHE_FLUSH_TS fence retires
 *      pass 1 (UCHE flushed) before pass 2's IB is even built, and pass 2 opens
 *      with CP_WAIT_FOR_IDLE + CP_EVENT_WRITE(CACHE_INVALIDATE) the way
 *      fd6_barrier_flush orders dependent compute launches.  The first-frame
 *      clears go through the A2D (adreno_2d_fill), itself fence-retired, before
 *      pass 1.  Whether anything else is needed on A690 is unvalidated.
 *
 *   7. GEOMETRY KERNEL I/O.  The kernels (projects/soft3d/.../geom_raster_cs.h)
 *      read their texture through an SSBO as linear ARGB words and write the
 *      framebuffer as 0xAARRGGBB; the same convention as on V3D.  Nothing about
 *      the a6xx texture unit is used.
 *
 * The GPU path is opt-in: the bsp layer gates these calls behind ADRENO_ENABLE
 * and a successful bring-up probe.  These functions return 0 (or a >=0 handle)
 * on success and a negative value when the program is not a valid Adreno blob,
 * the GPU is not up, the draw is ineligible, or the submission faulted/timed
 * out; the caller surfaces that and the client falls back to its in-process
 * engine.  There is no CPU path here.
 */
#include <adreno/adreno_drv.h>
#include <adreno/adreno_pm4.h>
#include <adreno/a6xx_regs.h>
#include <adreno/adreno_prog.h>
#include <ewoksys/klog.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* A690 (SC8280XP) device properties, from Mesa freedreno_devices.py    */
/* GPUId(690).  These drive the pvtmem sizing, the threadsize selection */
/* and the has_lpac register branches; they are NOT tunables.           */
/* ------------------------------------------------------------------ */
#define A690_NUM_SP_CORES            8u     /* num_ccu == num_sp_cores      */
#define A690_FIBERS_PER_SP           4096u  /* 128 * 2 * 16                 */
#define A690_INSTR_CACHE_SIZE        127u   /* a6xx_gen4 instr_cache_size   */
#define A690_HAS_LPAC                1      /* a6xx_gen4 has_lpac           */
#define A690_SUPPORTS_DOUBLE_THRSZ   1      /* a6xx_base supports_double_threadsize */

/* Maximum resident programs (small fixed table; portablegl keeps a handful). */
#define ADRENO_3D_MAX_PROGS          8

/* CP ring IB staging is 4096 dwords (adreno_ring.c RING_IB_SIZE); a dispatch
   larger than that cannot be submitted. */
#define ADRENO_3D_IB_MAX             4096u

/* Hard caps on blob-declared sizes so every offset/size sum in
   adreno_3d_program_load / adreno_3d_draw is provably wrap-free and the GPU
   BO requests stay sane.  A portablegl CS kernel is a few KB; 16MB of
   binary+constant data is far beyond any real program.  pvtmem is per fiber
   and multiplied by 4096 fibers x 8 SPs (32768x) for the BO, so 16KB per
   fiber already means a 512MB private buffer - anything above that is a
   corrupt header, not a shader. */
#define ADRENO_3D_MAX_BLOB_BYTES     (16u * 1024u * 1024u)
#define ADRENO_3D_MAX_PVTMEM_BYTES   (16u * 1024u)

/* Two-pass geometry rasterizer intermediate: `vout` holds one packed record
   per vertex, written by pass 1 and read by pass 2.  The gouraud / textured /
   instanced variants pack 8 floats (32 bytes); the colour-modulated textured
   variant (is_geom == 4, imgui) also carries a per-vertex colour, 12 floats
   (48 bytes).  Sized for the widest record and ADRENO_3D_MAX_VERTS vertices
   (same budget as raspi5's V3D_3D_MAX_VERTS / V3D_3D_VOUT_BYTES); a single
   process-lifetime buffer is safe because every draw is fence-retired before
   the next one is built and each geom draw fully overwrites the sub-range it
   consumes. */
#define ADRENO_3D_MAX_VERTS          20480u
#define ADRENO_3D_VOUT_BYTES         (ADRENO_3D_MAX_VERTS * 48u)

/* Buffers a dispatch may bind, indexed by the kernel's declared binding. */
#define ADRENO_3D_MAX_BINDINGS       ADRENO_PROG_MAX_SSBO

/* ------------------------------------------------------------------ */
/* Capability latch (mirrors adreno_2d_ready).                        */
/* ------------------------------------------------------------------ */
static int g_3d_ready = 0;   /* 0 = unprobed, 1 = ready, -1 = declined */

int adreno_3d_ready(void) {
	if (g_3d_ready == 0)
		g_3d_ready = (adreno_3d_init() == 0) ? 1 : -1;
	return g_3d_ready == 1;
}

/* ------------------------------------------------------------------ */
/* Indirect-buffer builder.  A CS dispatch is variable length (inline   */
/* immediates + pushed uniforms), so the dword array is heap-allocated  */
/* per dispatch at the ring's IB cap.  ib_w() never writes past cap but */
/* keeps counting, so `n > cap` after emission means "did not fit".     */
/* ------------------------------------------------------------------ */
struct ib {
	uint32_t *d;
	uint32_t  n;
	uint32_t  cap;
};

static void ib_w(struct ib *b, uint32_t v) {
	if (b->n < b->cap)
		b->d[b->n] = v;
	b->n++;
}

static void ib_pkt4(struct ib *b, uint32_t reg, uint32_t cnt) {
	ib_w(b, ADRENO_PKT4(reg, cnt));
}

static void ib_pkt7(struct ib *b, uint32_t opcode, uint32_t cnt) {
	ib_w(b, ADRENO_PKT7(opcode, cnt));
}

/* ------------------------------------------------------------------ */
/* Program table.  Each loaded program keeps, per kernel, a CPU copy of */
/* the blob header + immediates (replayed into the IB every dispatch)   */
/* and the GPU buffers (the assembled binary and, if it spills, private */
/* memory).                                                             */
/* ------------------------------------------------------------------ */
typedef struct {
	adreno_prog_kernel_t k;   /* CPU copy of the kernel header       */
	uint32_t    *imm;         /* CPU copy of imm consts, or NULL     */
	adreno_bo_t  bin_bo;      /* assembled binary (+ constant data)  */
	adreno_bo_t  pvt_bo;      /* private (spill/stack) memory, if any */
	uint32_t     per_fiber_size;
	uint32_t     per_sp_size;
} kern_slot_t;

typedef struct {
	int          in_use;
	uint8_t      is_geom;
	uint16_t     geom_tpw;
	uint32_t     cs2_uniform_blob_dwords;
	kern_slot_t  kern[2];     /* [0] main / raster, [1] xform (geom only) */
} prog_slot_t;

static prog_slot_t g_progs[ADRENO_3D_MAX_PROGS];

/* vout: allocated on the first geometry draw, kept for the process. */
static adreno_bo_t g_vout;

static uint32_t align_up(uint32_t v, uint32_t a) {
	return (v + (a - 1u)) & ~(a - 1u);
}

static uint32_t next_pow2(uint32_t v) {
	uint32_t p = 1u;
	if (v == 0u)
		return 0u;
	while (p < v)
		p <<= 1;
	return p;
}

static void kern_release(kern_slot_t *ks) {
	if (ks->imm) { free(ks->imm); ks->imm = 0; }
	adreno_bo_free(&ks->bin_bo);
	adreno_bo_free(&ks->pvt_bo);
	memset(ks, 0, sizeof(*ks));
}

/* Header sanity for one kernel; every sum is formed in 64 bits because the
   fields are attacker-controlled u32s.  Returns 0 when dispatchable. */
static int kern_validate(const adreno_prog_kernel_t *k) {
	uint32_t i;
	if (k->binary_words == 0 || k->instrlen == 0)
		return -1;
	if (k->binary_words > ADRENO_3D_MAX_BLOB_BYTES / 4u ||
	    k->imm_const_count > ADRENO_3D_MAX_BLOB_BYTES / 4u ||
	    k->constant_data_size > ADRENO_3D_MAX_BLOB_BYTES ||
	    k->constant_data_off > ADRENO_3D_MAX_BLOB_BYTES)
		return -1;
	if ((k->constant_data_size & 3u) != 0)
		return -1;
	if (k->local_size[0] == 0 || k->local_size[1] == 0 || k->local_size[2] == 0)
		return -1;
	if (k->local_size[0] > 1024u || k->local_size[1] > 1024u ||
	    k->local_size[2] > 1024u)
		return -1;   /* SP_CS_NDRANGE_0 fields are 10 bits (size - 1) */
	if (k->pvtmem_size > ADRENO_3D_MAX_PVTMEM_BYTES)
		return -1;
	if (k->constlen == 0 || k->constlen > 512u)
		return -1;   /* SP_CS_CONST_CONFIG.CONSTLEN budget (A6XX_CONSTLEN_512) */
	if (k->num_ssbo > ADRENO_PROG_MAX_SSBO ||
	    k->num_ubo_ranges > ADRENO_PROG_MAX_UBO_RANGES)
		return -1;
	for (i = 0; i < k->num_ssbo; i++)
		if (k->ssbo_binding[i] >= ADRENO_3D_MAX_BINDINGS)
			return -1;
	for (i = 0; i < k->num_ubo_ranges; i++) {
		const adreno_prog_ubo_range_t *r = &k->ubo_range[i];
		if ((r->src_byte & 3u) != 0 || (r->size_bytes & 15u) != 0 ||
		    r->size_bytes == 0)
			return -1;
		if ((uint64_t)r->src_byte + r->size_bytes > ADRENO_3D_MAX_BLOB_BYTES ||
		    r->dst_vec4 >= k->constlen)
			return -1;
	}
	if (k->driver_params_vec4_off != ADRENO_PROG_NO_DRIVER_PARAMS) {
		if (k->driver_params_dwords == 0 || k->driver_params_dwords > 16u ||
		    k->driver_params_vec4_off >= k->constlen)
			return -1;
	}
	if (k->num_ubos > 16u)
		return -1;
	if (k->consts_ubo_idx >= 0 && (uint32_t)k->consts_ubo_idx >= k->num_ubos)
		return -1;
	return 0;
}

static uint32_t kern_payload_bytes(const adreno_prog_kernel_t *k) {
	return k->binary_words * 4u + k->imm_const_count * 4u + k->constant_data_size;
}

/* Stage one kernel's payload sections into its GPU/CPU buffers. */
static int kern_load(kern_slot_t *ks, const adreno_prog_kernel_t *k,
                     const uint8_t *payload) {
	uint32_t bin_bytes = k->binary_words * 4u;
	uint32_t imm_bytes = k->imm_const_count * 4u;
	uint32_t cdata_bytes = k->constant_data_size;
	uint64_t need, cdata_end;
	uint32_t bo_size;

	memset(ks, 0, sizeof(*ks));
	ks->k = *k;

	/* --- GPU binary BO: the assembled ir3 binary, 128-byte aligned (the
	   tu6_emit_xs assert (binary_iova & 0x7f)==0).  The immutable constant
	   data lives at constant_data_off WITHIN this buffer (turnip reads
	   consts_ubo from binary_iova + v->info.constant_data_offset), so grow
	   the BO to cover it if the blob carries it as a separate section. --- */
	need = bin_bytes;
	cdata_end = (uint64_t)k->constant_data_off + cdata_bytes;   /* no wrap: both capped */
	if (cdata_bytes > 0 && cdata_end > need)
		need = cdata_end;
	if (need > ADRENO_3D_MAX_BLOB_BYTES)
		goto fail;
	bo_size = align_up((uint32_t)need, 128u);
	if (bo_size == 0 || bo_size < need || adreno_bo_alloc(&ks->bin_bo, bo_size) != 0)
		goto fail;
	ks->bin_bo.iova = ks->bin_bo.pa;   /* CB0 identity map */
	if ((ks->bin_bo.pa & 0x7fu) != 0) {
		klog("adreno_3d: bin iova %llx not 128-aligned\n",
		     (unsigned long long)ks->bin_bo.pa);
		goto fail;
	}
	memcpy(ks->bin_bo.va, payload, bin_bytes);
	if (cdata_bytes > 0) {
		if (cdata_end > bo_size)   /* cannot happen after the checks above; keep the memcpy provably in-BO */
			goto fail;
		memcpy((uint8_t *)ks->bin_bo.va + k->constant_data_off,
		       payload + bin_bytes + imm_bytes, cdata_bytes);
	}

	/* --- CPU copy of the immediates (replayed inline every dispatch) --- */
	if (k->imm_const_count > 0) {
		ks->imm = (uint32_t *)malloc(imm_bytes);
		if (ks->imm == 0)
			goto fail;
		memcpy(ks->imm, payload + bin_bytes, imm_bytes);
	}

	/* --- private (spill/stack) memory, tu6_get_pvtmem_config.  pvtmem_size
	   is capped above so the per-fiber * fibers * cores product fits u32. --- */
	if (k->pvtmem_size > 0) {
		uint64_t total;
		ks->per_fiber_size = next_pow2(align_up(k->pvtmem_size, 512u));
		ks->per_sp_size =
			align_up(ks->per_fiber_size * A690_FIBERS_PER_SP, 4096u);
		total = (uint64_t)A690_NUM_SP_CORES * ks->per_sp_size;
		if (total == 0 || total > 0xffffffffu ||
		    adreno_bo_alloc(&ks->pvt_bo, (uint32_t)total) != 0)
			goto fail;
		ks->pvt_bo.iova = ks->pvt_bo.pa;
		if ((ks->pvt_bo.pa & 0x1fu) != 0)   /* tu6_emit_xs assert */
			goto fail;
	}
	return 0;

fail:
	kern_release(ks);
	return -1;
}

/* ------------------------------------------------------------------ */
/* Load a program blob.  Returns a >=0 handle (the slot index) or a     */
/* negative value when the blob is not a valid Adreno program, the GPU   */
/* is not up, the table is full, or a BO allocation failed.  A V3D-QPU    */
/* blob (portablegl's default) fails the magic check and is declined.    */
/* ------------------------------------------------------------------ */
int adreno_3d_program_load(const void *blob, uint32_t size) {
	const adreno_prog_blob_t *h;
	const uint8_t *payload;
	prog_slot_t *s = 0;
	uint64_t total;
	int i, slot = -1, have_xform;

	if (!adreno_3d_ready())
		return -1;
	if (blob == 0 || size < sizeof(adreno_prog_blob_t))
		return -1;

	h = (const adreno_prog_blob_t *)blob;
	if (h->magic != ADRENO_PROG_MAGIC || h->version != ADRENO_PROG_VERSION)
		return -1;   /* not an Adreno blob (e.g. V3D-QPU) */

	/* --- header geometry.  kern[0] is mandatory; kern[1] (xform) must be
	   present exactly when the program is a two-pass geometry program. --- */
	if (kern_validate(&h->kern[0]) != 0)
		return -1;
	have_xform = (h->kern[1].binary_words != 0);
	if (h->is_geom > 4u)
		return -1;
	if (h->is_geom != 0 && !have_xform)
		return -1;
	if (h->is_geom == 0 && have_xform)
		return -1;
	if (have_xform && kern_validate(&h->kern[1]) != 0)
		return -1;
	/* The raster kernel maps `tri = gl_WorkGroupID.x * TPW + lane`, which is
	   only complete when TPW equals the workgroup lane count; a mismatched
	   tpw would silently render an incomplete image (raspi5 invariant). */
	if (h->is_geom != 0 && h->geom_tpw != 0 &&
	    (uint32_t)h->geom_tpw != h->kern[0].local_size[0] * h->kern[0].local_size[1])
		return -1;
	total = (uint64_t)kern_payload_bytes(&h->kern[0]);
	if (have_xform)
		total += kern_payload_bytes(&h->kern[1]);
	if (total != h->payload_bytes)
		return -1;
	if ((uint64_t)sizeof(adreno_prog_blob_t) + h->payload_bytes != size)
		return -1;

	payload = (const uint8_t *)blob + sizeof(adreno_prog_blob_t);

	/* --- find a free slot --- */
	for (i = 0; i < ADRENO_3D_MAX_PROGS; i++) {
		if (!g_progs[i].in_use) { slot = i; break; }
	}
	if (slot < 0)
		return -1;
	s = &g_progs[slot];
	memset(s, 0, sizeof(*s));
	s->is_geom = h->is_geom;
	s->geom_tpw = h->geom_tpw;
	s->cs2_uniform_blob_dwords = h->cs2_uniform_blob_dwords;

	if (kern_load(&s->kern[0], &h->kern[0], payload) != 0)
		goto fail;
	if (have_xform &&
	    kern_load(&s->kern[1], &h->kern[1],
	              payload + kern_payload_bytes(&h->kern[0])) != 0)
		goto fail;

	s->in_use = 1;
	return slot;

fail:
	kern_release(&s->kern[0]);
	kern_release(&s->kern[1]);
	memset(s, 0, sizeof(*s));
	return -1;
}

/* ------------------------------------------------------------------ */
/* Free a loaded program.  Returns 0 on success, negative on a bad       */
/* handle.  Idempotent for an already-free slot.                        */
/* ------------------------------------------------------------------ */
int adreno_3d_program_free(int32_t handle) {
	prog_slot_t *s;
	if (handle < 0 || handle >= ADRENO_3D_MAX_PROGS)
		return -1;
	s = &g_progs[handle];
	if (!s->in_use)
		return 0;
	kern_release(&s->kern[0]);
	kern_release(&s->kern[1]);
	memset(s, 0, sizeof(*s));
	return 0;
}

/* ------------------------------------------------------------------ */
/* Build a linear R32_UINT a6xx_tex_const (MEMOBJ) buffer view over a     */
/* GPU buffer, exactly like fdl6_buffer_view_init(PIPE_FORMAT_R32_UINT,  */
/* TILE6_LINEAR, no ssbo_emulation).  `desc` points at 16 dwords.  The    */
/* base must be 64-byte aligned so STARTOFFSETTEXELS is 0 (the caller     */
/* declines an unaligned buffer).  elements = total u32 texels = size/4.  */
/* ------------------------------------------------------------------ */
static void build_memobj(uint32_t *desc, uint64_t base_iova, uint32_t elements) {
	uint64_t aligned = base_iova & ~0x3full;
	memset(desc, 0, A6XX_TEX_CONST_DWORDS * 4u);
	desc[0] = A6XX_TEX_MEMOBJ_0_TILE_MODE(TILE6_LINEAR) |
	          A6XX_TEX_MEMOBJ_0_SWAP(A3XX_COLOR_SWAP_WZYX) |
	          A6XX_TEX_MEMOBJ_0_FMT(FMT6_32_UINT) |
	          A6XX_TEX_MEMOBJ_0_MIPLVLS(0) |
	          A6XX_TEX_MEMOBJ_0_SWIZ_X(A6XX_TEX_SWIZ_X) |
	          A6XX_TEX_MEMOBJ_0_SWIZ_Y(A6XX_TEX_SWIZ_Y) |
	          A6XX_TEX_MEMOBJ_0_SWIZ_Z(A6XX_TEX_SWIZ_Z) |
	          A6XX_TEX_MEMOBJ_0_SWIZ_W(A6XX_TEX_SWIZ_W);
	desc[1] = A6XX_TEX_MEMOBJ_1_WIDTH(elements & 0x7fffu) |
	          A6XX_TEX_MEMOBJ_1_HEIGHT((elements >> 15) & 0x7fffu);
	desc[2] = A6XX_TEX_MEMOBJ_2_STRUCTSIZETEXELS(1) |
	          A6XX_TEX_MEMOBJ_2_STARTOFFSETTEXELS(0) |
	          A6XX_TEX_MEMOBJ_2_TYPE(A6XX_TEX_BUFFER);
	desc[4] = (uint32_t)(aligned & 0xffffffffu);
	desc[5] = (uint32_t)(aligned >> 32);
	/* desc[3], desc[6..15] stay 0 (no ssbo_emulation on A690). */
}

/* ------------------------------------------------------------------ */
/* One CS dispatch.                                                      */
/* ------------------------------------------------------------------ */

/* A buffer the request can satisfy, by declared binding. */
typedef struct {
	uint64_t phy;
	uint32_t bytes;
} bind_t;

/* A per-dispatch descriptor BO whose dispatch never retired.  The GPU may still
   be reading it, so it is deliberately leaked (the device is wedged at that
   point, so this happens at most once per boot). */
static int g_leaked_desc = 0;

/* Per-dispatch BO layout: [UAV descriptors: num_ssbo x 64 B][UBO-0 copy of
   the kernel's uniform region, 64-byte aligned, >= 64 B]. */
#define DESC_UBO_MIN_BYTES  64u

/* Emit and submit one compute dispatch of kernel `ks` over grid[3]
   workgroups, binding the SSBO slots from `binds` (indexed by declared
   binding) and feeding `unif[num_uniforms]` as UBO 0.  `barrier` prepends the
   inter-dispatch flush (pass 2 of the geometry path).  Returns ADRENO_OK,
   ADRENO_E_DECLINED before anything reached the GPU, or the adreno_ring_submit
   result (TIMEOUT / FAULT / WEDGED). */
static int emit_cs_dispatch(const kern_slot_t *ks, const bind_t *binds,
                            uint32_t nbinds, const uint32_t *unif,
                            uint32_t num_uniforms, const uint32_t grid[3],
                            int barrier) {
	const adreno_prog_kernel_t *k = &ks->k;
	struct ib b;
	adreno_bo_t desc_bo;
	uint32_t bin_iova_lo, bin_iova_hi, pvt_lo, pvt_hi;
	uint64_t bin_iova, pvt_iova, ubo_iova;
	uint32_t thrsz, thrsz_cs, shared_size, cram, preload;
	uint32_t gsx, gsy, gsz;
	uint32_t desc_bytes, ubo_off, ubo_bytes, ubo_vec4s, ubo_region_dwords;
	uint32_t i;
	int ret;

	if (unif == 0 && num_uniforms > 0)
		return ADRENO_E_DECLINED;
	if (num_uniforms > ADRENO_3D_IB_MAX)   /* can never fit the IB anyway */
		return ADRENO_E_DECLINED;
	if (grid[0] == 0 || grid[1] == 0 || grid[2] == 0)
		return ADRENO_E_DECLINED;
	/* SP_CS_NDRANGE globalsize = local*groups must not wrap */
	if ((uint64_t)grid[0] * k->local_size[0] > 0xffffffffu ||
	    (uint64_t)grid[1] * k->local_size[1] > 0xffffffffu ||
	    (uint64_t)grid[2] * k->local_size[2] > 0xffffffffu)
		return ADRENO_E_DECLINED;
	gsx = k->local_size[0] * grid[0];
	gsy = k->local_size[1] * grid[1];
	gsz = k->local_size[2] * grid[2];

	/* every UAV slot the kernel addresses must be satisfiable */
	for (i = 0; i < k->num_ssbo; i++) {
		uint32_t bnd = k->ssbo_binding[i];
		if (bnd >= nbinds || binds[bnd].phy == 0 || binds[bnd].bytes < 4u)
			return ADRENO_E_DECLINED;
		if ((binds[bnd].phy & 0x3fu) != 0)   /* MEMOBJ base must be 64-aligned */
			return ADRENO_E_DECLINED;
	}

	/* --- threadsize / shared / const-ram mode --- */
	thrsz = k->double_threadsize ? A6XX_THREAD128 : A6XX_THREAD64;
	thrsz_cs = A690_SUPPORTS_DOUBLE_THRSZ ? thrsz : A6XX_THREAD128;
	shared_size = ((int)k->shared_size - 1) / 1024;
	if ((int)shared_size < 1)
		shared_size = 1;
	shared_size = shared_size & 0x1fu;
	cram = k->constlen > 256 ? A6XX_CONSTLEN_512 :
	       k->constlen > 192 ? A6XX_CONSTLEN_256 :
	       k->constlen > 128 ? A6XX_CONSTLEN_192 : A6XX_CONSTLEN_128;
	preload = k->instrlen < A690_INSTR_CACHE_SIZE ? k->instrlen
	                                              : A690_INSTR_CACHE_SIZE;

	/* --- per-dispatch BO: UAV descriptors + the UBO-0 uniform copy --- */
	ubo_region_dwords = num_uniforms < k->uniform_blob_dwords ? num_uniforms
	                                                          : k->uniform_blob_dwords;
	desc_bytes = k->num_ssbo * A6XX_TEX_CONST_DWORDS * 4u;
	ubo_off = align_up(desc_bytes, 64u);
	ubo_bytes = align_up(k->uniform_blob_dwords * 4u, 16u);
	if (ubo_bytes < DESC_UBO_MIN_BYTES)
		ubo_bytes = DESC_UBO_MIN_BYTES;
	if (adreno_bo_alloc(&desc_bo, ubo_off + ubo_bytes) != 0)
		return ADRENO_E_DECLINED;
	desc_bo.iova = desc_bo.pa;
	if ((desc_bo.pa & 0x3fu) != 0) {
		adreno_bo_free(&desc_bo);
		return ADRENO_E_DECLINED;
	}
	for (i = 0; i < k->num_ssbo; i++) {
		const bind_t *bd = &binds[k->ssbo_binding[i]];
		build_memobj((uint32_t *)desc_bo.va + i * A6XX_TEX_CONST_DWORDS,
		             bd->phy, bd->bytes / 4u);
	}
	if (ubo_region_dwords > 0)
		memcpy((uint8_t *)desc_bo.va + ubo_off, unif, ubo_region_dwords * 4u);
	ubo_iova = desc_bo.iova + ubo_off;
	ubo_vec4s = ubo_bytes / 16u;

	/* --- allocate the IB at the ring cap; ib_w counts past it --- */
	b.d = (uint32_t *)malloc(ADRENO_3D_IB_MAX * 4u);
	if (b.d == 0) {
		adreno_bo_free(&desc_bo);
		return ADRENO_E_DECLINED;
	}
	b.n = 0; b.cap = ADRENO_3D_IB_MAX;

	bin_iova = ks->bin_bo.iova;
	bin_iova_lo = (uint32_t)(bin_iova & 0xffffffffu);
	bin_iova_hi = (uint32_t)(bin_iova >> 32);
	pvt_iova = ks->pvt_bo.va ? ks->pvt_bo.iova : 0ull;
	pvt_lo = (uint32_t)(pvt_iova & 0xffffffffu);
	pvt_hi = (uint32_t)(pvt_iova >> 32);

	/* === inter-dispatch barrier (geometry pass 2): the previous dispatch
	   already retired through the ring fence (CACHE_FLUSH_TS); drain the
	   pipeline and invalidate UCHE before this one reads what it wrote,
	   the way fd6_barrier_flush orders dependent launches === */
	if (barrier) {
		ib_pkt7(&b, CP_WAIT_FOR_IDLE, 0);
		ib_pkt7(&b, CP_EVENT_WRITE, 1);
		ib_w(&b, CACHE_INVALIDATE);
	}

	/* === dispatch prologue: enter the COMPUTE render mode === */
	ib_pkt7(&b, CP_SET_MARKER, 1);
	ib_w(&b, A6XX_SET_MARKER_COMPUTE);

	/* === 1. SP_UPDATE_CNTL(cs_state | cs_uav) === */
	ib_pkt4(&b, REG_A6XX_SP_UPDATE_CNTL, 1);
	ib_w(&b, A6XX_SP_UPDATE_CNTL_CS_STATE | A6XX_SP_UPDATE_CNTL_CS_UAV);

	/* === 2. SP_CS_CONST_CONFIG(constlen,enabled) + SP_CS_CONFIG === */
	ib_pkt4(&b, REG_A6XX_SP_CS_CONST_CONFIG, 1);
	ib_w(&b, A6XX_SP_CS_CONST_CONFIG_CONSTLEN(k->constlen) |
	         A6XX_SP_CS_CONST_CONFIG_ENABLED);
	ib_pkt4(&b, REG_A6XX_SP_CS_CONFIG, 1);
	ib_w(&b, A6XX_SP_CS_CONFIG_ENABLED |
	         A6XX_SP_CS_CONFIG_NTEX(0) |
	         A6XX_SP_CS_CONFIG_NSAMP(0) |
	         A6XX_SP_CS_CONFIG_NUAV(k->num_ssbo));   /* ir3_shader_num_uavs */

	/* === 3. tu6_emit_xs(COMPUTE): CNTL_0 + program/pvtmem layout === */
	ib_pkt4(&b, REG_A6XX_SP_CS_CNTL_0, 1);
	ib_w(&b, A6XX_SP_CS_CNTL_0_HALFREGFOOTPRINT(k->max_half_reg + 1u) |
	         A6XX_SP_CS_CNTL_0_FULLREGFOOTPRINT(k->max_reg + 1u) |
	         A6XX_SP_CS_CNTL_0_BRANCHSTACK(k->branchstack_hw) |
	         A6XX_SP_CS_CNTL_0_THREADSIZE(thrsz) |
	         (k->round_robin_mode ? A6XX_SP_CS_CNTL_0_COMPUTERRMODEEN : 0u) |
	         (k->early_preamble   ? A6XX_SP_CS_CNTL_0_EARLYPREAMBLE   : 0u) |
	         (k->mergedregs       ? A6XX_SP_CS_CNTL_0_MERGEDREGS      : 0u));
	ib_pkt4(&b, REG_A6XX_SP_CS_INSTR_SIZE, 1);
	ib_w(&b, k->instrlen);
	ib_pkt4(&b, REG_A6XX_SP_CS_PROGRAM_COUNTER_OFFSET, 1);
	ib_w(&b, 0);
	ib_pkt4(&b, REG_A6XX_SP_CS_BASE, 2);
	ib_w(&b, bin_iova_lo);
	ib_w(&b, bin_iova_hi);
	ib_pkt4(&b, REG_A6XX_SP_CS_PVT_MEM_PARAM, 1);
	ib_w(&b, A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM(ks->per_fiber_size));
	ib_pkt4(&b, REG_A6XX_SP_CS_PVT_MEM_BASE, 2);
	ib_w(&b, pvt_lo);
	ib_w(&b, pvt_hi);
	ib_pkt4(&b, REG_A6XX_SP_CS_PVT_MEM_SIZE, 1);
	ib_w(&b, A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(ks->per_sp_size) |
	         (k->pvtmem_per_wave ? A6XX_SP_CS_PVT_MEM_SIZE_PERWAVEMEMLAYOUT : 0u));
	ib_pkt4(&b, REG_A6XX_SP_CS_PVT_MEM_STACK_OFFSET, 1);
	ib_w(&b, A6XX_SP_CS_PVT_MEM_STACK_OFFSET_OFFSET(ks->per_sp_size));

	/* === 4. shader preload (SS6_INDIRECT) + 5. immediates
	   (tu6_emit_xs_constants).  All use CP_LOAD_STATE6_FRAG (the COMPUTE
	   opcode, tu6_stage2opcode) with SB6_CS_SHADER. === */
	ib_pkt7(&b, CP_LOAD_STATE6_FRAG, 3);
	ib_w(&b, A6XX_CP_LOAD_STATE6_0_DST_OFF(0) |
	         A6XX_CP_LOAD_STATE6_0_STATE_TYPE(A6XX_ST6_SHADER) |
	         A6XX_CP_LOAD_STATE6_0_STATE_SRC(A6XX_SS6_INDIRECT) |
	         A6XX_CP_LOAD_STATE6_0_STATE_BLOCK(A6XX_SB6_CS_SHADER) |
	         A6XX_CP_LOAD_STATE6_0_NUM_UNIT(preload));
	ib_w(&b, bin_iova_lo);          /* emit_qw(binary_iova) */
	ib_w(&b, bin_iova_hi);

	if (k->imm_const_count > 0 && ks->imm != 0) {
		/* vec4 count, truncated to the const budget the way
		   tu6_emit_xs_constants does (size = MIN(size + base, constlen) - base). */
		uint32_t vec4s = (k->imm_const_count + 3u) / 4u;
		if (vec4s + k->imm_const_base > k->constlen)
			vec4s = k->constlen > k->imm_const_base ? k->constlen - k->imm_const_base : 0u;
		if (vec4s > 0) {
			ib_pkt7(&b, CP_LOAD_STATE6_FRAG, 3 + vec4s * 4u);
			ib_w(&b, A6XX_CP_LOAD_STATE6_0_DST_OFF(k->imm_const_base) |
			         A6XX_CP_LOAD_STATE6_0_STATE_TYPE(A6XX_ST6_CONSTANTS) |
			         A6XX_CP_LOAD_STATE6_0_STATE_SRC(A6XX_SS6_DIRECT) |
			         A6XX_CP_LOAD_STATE6_0_STATE_BLOCK(A6XX_SB6_CS_SHADER) |
			         A6XX_CP_LOAD_STATE6_0_NUM_UNIT(vec4s));
			ib_w(&b, A6XX_CP_LOAD_STATE6_1_EXT_SRC_ADDR(0));
			ib_w(&b, A6XX_CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(0));
			for (i = 0; i < vec4s * 4u; i++)
				ib_w(&b, i < k->imm_const_count ? ks->imm[i] : 0u);
		}
	}

	/* === 6. UBO descriptors (fd6_emit_ubos): one ST6_UBO upload covering
	   [0, num_ubos), 2 dwords each (addr lo | addr hi + SIZE in vec4s).
	   Descriptor 0 is the per-dispatch uniform copy (ldc fallback for any
	   range ir3 did not push), consts_ubo_idx the immutable constant data
	   inside the binary BO; any other index is left null. === */
	if (k->num_ubos > 0) {
		ib_pkt7(&b, CP_LOAD_STATE6_FRAG, 3 + k->num_ubos * 2u);
		ib_w(&b, A6XX_CP_LOAD_STATE6_0_DST_OFF(0) |
		         A6XX_CP_LOAD_STATE6_0_STATE_TYPE(A6XX_ST6_UBO) |
		         A6XX_CP_LOAD_STATE6_0_STATE_SRC(A6XX_SS6_DIRECT) |
		         A6XX_CP_LOAD_STATE6_0_STATE_BLOCK(A6XX_SB6_CS_SHADER) |
		         A6XX_CP_LOAD_STATE6_0_NUM_UNIT(k->num_ubos));
		ib_w(&b, A6XX_CP_LOAD_STATE6_1_EXT_SRC_ADDR(0));
		ib_w(&b, A6XX_CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(0));
		for (i = 0; i < k->num_ubos; i++) {
			if (i == 0) {
				ib_w(&b, (uint32_t)(ubo_iova & 0xffffffffu));
				ib_w(&b, (uint32_t)((ubo_iova >> 32) & 0xffffffffu) |
				         A6XX_UBO_1_SIZE(ubo_vec4s));
			} else if (k->consts_ubo_idx >= 0 && i == (uint32_t)k->consts_ubo_idx &&
			           k->constant_data_size > 0) {
				uint64_t cd_iova = bin_iova + k->constant_data_off;
				ib_w(&b, (uint32_t)(cd_iova & 0xffffffffu));
				ib_w(&b, (uint32_t)((cd_iova >> 32) & 0xffffffffu) |
				         A6XX_UBO_1_SIZE((k->constant_data_size + 15u) / 16u));
			} else {
				ib_w(&b, 0);
				ib_w(&b, 0);
			}
		}
	}

	/* === 7. pushed uniform ranges (ir3_emit_user_consts): each ubo_range
	   copies size_bytes of the UBO-0 blob from src_byte into the constant
	   file at dst_vec4, clamped to the const budget; bytes the request did
	   not supply read as 0. === */
	for (i = 0; i < k->num_ubo_ranges; i++) {
		const adreno_prog_ubo_range_t *r = &k->ubo_range[i];
		uint32_t vec4s = r->size_bytes / 16u;
		uint32_t j, src_dw = r->src_byte / 4u;
		if (vec4s + r->dst_vec4 > k->constlen)
			vec4s = k->constlen - r->dst_vec4;   /* dst_vec4 < constlen: validated */
		if (vec4s == 0)
			continue;
		ib_pkt7(&b, CP_LOAD_STATE6_FRAG, 3 + vec4s * 4u);
		ib_w(&b, A6XX_CP_LOAD_STATE6_0_DST_OFF(r->dst_vec4) |
		         A6XX_CP_LOAD_STATE6_0_STATE_TYPE(A6XX_ST6_CONSTANTS) |
		         A6XX_CP_LOAD_STATE6_0_STATE_SRC(A6XX_SS6_DIRECT) |
		         A6XX_CP_LOAD_STATE6_0_STATE_BLOCK(A6XX_SB6_CS_SHADER) |
		         A6XX_CP_LOAD_STATE6_0_NUM_UNIT(vec4s));
		ib_w(&b, A6XX_CP_LOAD_STATE6_1_EXT_SRC_ADDR(0));
		ib_w(&b, A6XX_CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(0));
		for (j = 0; j < vec4s * 4u; j++) {
			uint32_t idx = src_dw + j;
			ib_w(&b, idx < ubo_region_dwords ? unif[idx] : 0u);
		}
	}

	/* === 8. driver params (struct ir3_driver_params_cs) === */
	if (k->driver_params_vec4_off != ADRENO_PROG_NO_DRIVER_PARAMS) {
		uint32_t dp[16];
		uint32_t vec4s = (k->driver_params_dwords + 3u) / 4u;
		uint32_t sg = k->double_threadsize ? 128u : 64u;
		memset(dp, 0, sizeof(dp));
		dp[0] = grid[0]; dp[1] = grid[1]; dp[2] = grid[2]; dp[3] = 3u; /* num_work_groups, work_dim */
		dp[4] = 0; dp[5] = 0; dp[6] = 0; dp[7] = sg;                  /* base_group, subgroup_size */
		dp[8] = k->local_size[0]; dp[9] = k->local_size[1]; dp[10] = k->local_size[2];
		dp[11] = (sg == 128u) ? 7u : 6u;                              /* subgroup_id_shift */
		/* dp[12..15]: workgroup_id (indirect only) + pad stay 0 */
		if (vec4s + k->driver_params_vec4_off > k->constlen)
			vec4s = k->constlen - k->driver_params_vec4_off;
		if (vec4s > 0) {
			ib_pkt7(&b, CP_LOAD_STATE6_FRAG, 3 + vec4s * 4u);
			ib_w(&b, A6XX_CP_LOAD_STATE6_0_DST_OFF(k->driver_params_vec4_off) |
			         A6XX_CP_LOAD_STATE6_0_STATE_TYPE(A6XX_ST6_CONSTANTS) |
			         A6XX_CP_LOAD_STATE6_0_STATE_SRC(A6XX_SS6_DIRECT) |
			         A6XX_CP_LOAD_STATE6_0_STATE_BLOCK(A6XX_SB6_CS_SHADER) |
			         A6XX_CP_LOAD_STATE6_0_NUM_UNIT(vec4s));
			ib_w(&b, A6XX_CP_LOAD_STATE6_1_EXT_SRC_ADDR(0));
			ib_w(&b, A6XX_CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(0));
			for (i = 0; i < vec4s * 4u; i++)
				ib_w(&b, dp[i]);
		}
	}

	/* === 9. SSBO -> UAV descriptors (fd6_emit_cs_state, non-bindless):
	   ST6_UAV SS6_INDIRECT from the per-dispatch BO, then SP_CS_UAV_BASE +
	   SP_CS_USIZE (gap #1) === */
	if (k->num_ssbo > 0) {
		ib_pkt7(&b, CP_LOAD_STATE6_FRAG, 3);
		ib_w(&b, A6XX_CP_LOAD_STATE6_0_DST_OFF(0) |
		         A6XX_CP_LOAD_STATE6_0_STATE_TYPE(A6XX_ST6_UAV) |
		         A6XX_CP_LOAD_STATE6_0_STATE_SRC(A6XX_SS6_INDIRECT) |
		         A6XX_CP_LOAD_STATE6_0_STATE_BLOCK(A6XX_SB6_CS_SHADER) |
		         A6XX_CP_LOAD_STATE6_0_NUM_UNIT(k->num_ssbo));
		ib_w(&b, (uint32_t)(desc_bo.iova & 0xffffffffu));
		ib_w(&b, (uint32_t)(desc_bo.iova >> 32));
		ib_pkt4(&b, REG_A6XX_SP_CS_UAV_BASE, 2);
		ib_w(&b, (uint32_t)(desc_bo.iova & 0xffffffffu));
		ib_w(&b, (uint32_t)(desc_bo.iova >> 32));
		ib_pkt4(&b, REG_A6XX_SP_CS_USIZE, 1);
		ib_w(&b, k->num_ssbo);
	}

	/* === 10. SP_CS_CNTL_1 + HLSQ_CS_CTRL_REG1 (A690 has_lpac) === */
	ib_pkt4(&b, REG_A6XX_SP_CS_CNTL_1, 1);
	ib_w(&b, A6XX_SP_CS_CNTL_1_SHARED_SIZE(shared_size) |
	         A6XX_SP_CS_CNTL_1_CONSTANTRAMMODE(cram));
#if A690_HAS_LPAC
	ib_pkt4(&b, REG_A6XX_HLSQ_CS_CTRL_REG1, 1);
	ib_w(&b, A6XX_HLSQ_CS_CTRL_REG1_SHARED_SIZE(shared_size) |
	         A6XX_HLSQ_CS_CTRL_REG1_CONSTANTRAMMODE(cram));
#endif

	/* === 11. SP_CS_CONST_CONFIG_0 + SP_CS_WGE_CNTL === */
	ib_pkt4(&b, REG_A6XX_SP_CS_CONST_CONFIG_0, 1);
	ib_w(&b, A6XX_SP_CS_CONSTID_WGID(k->work_group_id_regid) |
	         A6XX_SP_CS_CONSTID_WGSIZE(A6XX_INVALID_REGID) |
	         A6XX_SP_CS_CONSTID_WGOFFSET(A6XX_INVALID_REGID) |
	         A6XX_SP_CS_CONSTID_LOCALID(k->local_invocation_id_regid));
	ib_pkt4(&b, REG_A6XX_SP_CS_WGE_CNTL, 1);
	ib_w(&b, A6XX_SP_CS_WGE_LINEARLOCALIDREGID(A6XX_INVALID_REGID) |
	         A6XX_SP_CS_WGE_THREADSIZE(thrsz_cs));

	/* === 12. SP_PS_WAVE_CNTL: SKIPPED (A690 supports_double_threadsize,
	   so the threadsize comes from SP_CS_WGE_CNTL, tu_shader.cc) === */

	/* === 13. SP_CS_WIE_CNTL_0 + _1 (A690 has_lpac) === */
#if A690_HAS_LPAC
	ib_pkt4(&b, REG_A6XX_SP_CS_WIE_CNTL_0, 1);
	ib_w(&b, A6XX_SP_CS_CONSTID_WGID(k->work_group_id_regid) |
	         A6XX_SP_CS_CONSTID_WGSIZE(A6XX_INVALID_REGID) |
	         A6XX_SP_CS_CONSTID_WGOFFSET(A6XX_INVALID_REGID) |
	         A6XX_SP_CS_CONSTID_LOCALID(k->local_invocation_id_regid));
	ib_pkt4(&b, REG_A6XX_SP_CS_WIE_CNTL_1, 1);
	ib_w(&b, A6XX_SP_CS_WGE_LINEARLOCALIDREGID(A6XX_INVALID_REGID) |
	         A6XX_SP_CS_WGE_THREADSIZE(thrsz));
#endif

	/* === 14. SP_CS_NDRANGE_0..6 (7 consecutive registers, b990..b996).
	   A6XX carries the local size in NDRANGE_0; NDRANGE_7 is A7XX+. === */
	ib_pkt4(&b, REG_A6XX_SP_CS_NDRANGE_0, 7);
	ib_w(&b, A6XX_SP_CS_NDRANGE_0_KERNELDIM(3) |
	         A6XX_SP_CS_NDRANGE_0_LOCALSIZEX(k->local_size[0] - 1u) |
	         A6XX_SP_CS_NDRANGE_0_LOCALSIZEY(k->local_size[1] - 1u) |
	         A6XX_SP_CS_NDRANGE_0_LOCALSIZEZ(k->local_size[2] - 1u));
	ib_w(&b, gsx);   /* NDRANGE_1 globalsize_x */
	ib_w(&b, 0);     /* NDRANGE_2 globaloff_x  */
	ib_w(&b, gsy);   /* NDRANGE_3 globalsize_y */
	ib_w(&b, 0);     /* NDRANGE_4 globaloff_y  */
	ib_w(&b, gsz);   /* NDRANGE_5 globalsize_z */
	ib_w(&b, 0);     /* NDRANGE_6 globaloff_z  */

	/* === 15. CP_EXEC_CS: 0, NGROUPS_X, NGROUPS_Y, NGROUPS_Z === */
	ib_pkt7(&b, CP_EXEC_CS, 4);
	ib_w(&b, 0);
	ib_w(&b, grid[0]);
	ib_w(&b, grid[1]);
	ib_w(&b, grid[2]);

	if (b.n > b.cap) {   /* did not fit the ring IB: nothing was submitted */
		free(b.d);
		adreno_bo_free(&desc_bo);
		return ADRENO_E_DECLINED;
	}

	ret = adreno_ring_submit(adreno_dev(), b.d, b.n);
	free(b.d);
	if (ret == ADRENO_OK || ret == ADRENO_E_DECLINED || ret == ADRENO_E_WEDGED) {
		/* OK: submit's fence retired.  DECLINED / WEDGED: nothing reached
		   the GPU.  Either way the per-dispatch BO is ours to free. */
		adreno_bo_free(&desc_bo);
		return ret;
	}
	/* TIMEOUT / FAULT: the dispatch was handed to the CP and never retired.
	   desc_bo may still be referenced by the (hung) GPU, so it must not be
	   returned to the allocator where the next user would see GPU writes. */
	if (!g_leaked_desc) {
		g_leaked_desc = 1;
		klog("adreno_3d: leaking desc BO pa=%llx (%u B) after submit failure %d\n",
		     (unsigned long long)desc_bo.pa, desc_bo.size, ret);
	}
	klog("adreno_3d: dispatch submit failed (%d)\n", ret);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Two-pass geometry rasterizer (mirrors raspi5 draw_compute_geom).    */
/* ------------------------------------------------------------------ */
static int draw_geom(const prog_slot_t *s, uint64_t fb_phy, uint32_t w,
                     uint32_t h, uint32_t fb_stride,
                     const adreno_3d_geom_t *g, const uint32_t *unif,
                     uint32_t num_uniforms) {
	const kern_slot_t *xf = &s->kern[1];
	const kern_slot_t *rs = &s->kern[0];
	bind_t binds[ADRENO_3D_MAX_BINDINGS];
	uint32_t grid[3];
	uint32_t ntris, split = s->cs2_uniform_blob_dwords;
	uint32_t xwg, tpw, pstride, rec;
	int gtex = (s->is_geom == 2), ginst = (s->is_geom == 3), gtc = (s->is_geom == 4);
	int gtexc = gtex || gtc;   /* textured family: ctex unit 0 is the image */
	int rc;

	if (g == 0)
		return ADRENO_E_DECLINED;
	/* is_geom == 2: pos carries interleaved pos+uv (6 floats); 3: vec4
	   pos+instance index + float3 normals, ctex unit 0 = instance-matrix
	   SSBO; 4: interleaved pos+uv+rgba (10 floats); 1: float3 pos + float3
	   normals.  The vout record is 12 floats for kind 4, 8 otherwise. */
	pstride = gtc ? 40u : (gtex ? 24u : (ginst ? 16u : 12u));
	rec = gtc ? 48u : 32u;
	ntris = g->nverts / 3u;

	if (g->mode != 4u)   /* G2D_PGL_MODE_TRIANGLES */
		return ADRENO_E_DECLINED;
	if (g->nverts == 0 || (g->nverts % 3u) != 0 || ntris >= (1u << 16))
		return ADRENO_E_DECLINED;
	if (g->nverts > ADRENO_3D_MAX_VERTS)
		return ADRENO_E_DECLINED;
	if (g->pos_phy == 0 || (g->pos_phy & 0x3fu) != 0)
		return ADRENO_E_DECLINED;
	if (!gtexc && (g->col_phy == 0 || (g->col_phy & 0x3fu) != 0))
		return ADRENO_E_DECLINED;
	if ((gtexc || ginst) &&
	    (g->ctex_phy == 0 || (g->ctex_phy & 0x3fu) != 0 ||
	     g->ctex_w == 0 || g->ctex_h == 0 ||
	     g->ctex_stride < g->ctex_w * 4u || (g->ctex_stride & 3u) != 0))
		return ADRENO_E_DECLINED;
	if (g->z_phy == 0 || (g->z_phy & 0x3fu) != 0)
		return ADRENO_E_DECLINED;
	if ((g->z_stride & 3u) != 0 || g->z_stride < w * 4u)
		return ADRENO_E_DECLINED;
	if (rs->k.local_size[2] != 1u || xf->k.local_size[2] != 1u)
		return ADRENO_E_DECLINED;
	if (split > num_uniforms)
		return ADRENO_E_DECLINED;
	/* The A2D frame clear fills whole rows with a 64-byte-aligned pitch; a
	   pitched colour or depth canvas would be left half-cleared, so decline
	   it and the client keeps its own CPU clear (pgl_g3d always sends w*4). */
	if (g->first_frame &&
	    (fb_stride != w * 4u || g->z_stride != w * 4u || (fb_stride & 0x3fu) != 0))
		return ADRENO_E_DECLINED;

	/* vout: process-lifetime intermediate, allocated on first use */
	if (g_vout.va == 0) {
		if (adreno_bo_alloc(&g_vout, ADRENO_3D_VOUT_BYTES) != 0)
			return ADRENO_E_DECLINED;
		g_vout.iova = g_vout.pa;
		if ((g_vout.pa & 0x3fu) != 0) {
			adreno_bo_free(&g_vout);
			return ADRENO_E_DECLINED;
		}
	}

	/* Frame clear on the GPU, before either pass: the client drops its
	   per-frame CPU clear of colour+depth once this path has run, so the
	   frame's first geom draw fills both canvases itself.  The geom fb
	   layout is 0xAARRGGBB while clear_rgba is portablegl's 0xAABBGGRR, so
	   R and B swap; depth clears to the atomicMin identity. */
	if (g->first_frame) {
		uint32_t c = g->clear_rgba;
		uint32_t px = (c & 0xFF000000u) | ((c & 0x000000FFu) << 16) |
		              (c & 0x0000FF00u) | ((c & 0x00FF0000u) >> 16);
		rc = adreno_2d_fill(fb_phy, (int32_t)w, (int32_t)h, fb_stride,
		                    0, 0, (int32_t)w, (int32_t)h, px, 0);
		if (rc != ADRENO_OK)
			return rc;
		rc = adreno_2d_fill(g->z_phy, (int32_t)w, (int32_t)h, g->z_stride,
		                    0, 0, (int32_t)w, (int32_t)h, 0xFFFFFFFFu, 0);
		if (rc != ADRENO_OK)
			return rc;
	}

	/* ---- Pass 1: vertex transform -> vout.  Bindings: 0 = vout, 1 = pos;
	   gouraud 2 = normals; instanced 2 = normals, 3 = instance matrices;
	   textured kinds read interleaved pos+uv from binding 1 only. ---- */
	memset(binds, 0, sizeof(binds));
	binds[0].phy = g_vout.iova;   binds[0].bytes = g->nverts * rec;
	binds[1].phy = g->pos_phy;    binds[1].bytes = g->nverts * pstride;
	if (!gtexc) {
		binds[2].phy = g->col_phy; binds[2].bytes = g->nverts * 12u;
	}
	if (ginst) {
		binds[3].phy = g->ctex_phy; binds[3].bytes = g->ctex_stride * g->ctex_h;
	}
	xwg = xf->k.local_size[0] * xf->k.local_size[1];
	grid[0] = (g->nverts + xwg - 1u) / xwg;   /* one invocation per vertex */
	grid[1] = 1u;
	grid[2] = 1u;
	rc = emit_cs_dispatch(xf, binds, 4u, unif, split, grid, 0);
	if (rc != ADRENO_OK) {
		klog("adreno_3d: geom xform rc=%d nverts %u wg=%u\n", rc, g->nverts, xwg);
		return rc;
	}

	/* ---- Pass 2: rasterize from vout.  Bindings: 0 = fb, 1 = vout,
	   2 = depth; textured family 3 = texture image.  The raster kernel's
	   uniforms live in the SECOND region of the blob. ---- */
	memset(binds, 0, sizeof(binds));
	binds[0].phy = fb_phy;        binds[0].bytes = fb_stride * h;
	binds[1].phy = g_vout.iova;   binds[1].bytes = g->nverts * rec;
	binds[2].phy = g->z_phy;      binds[2].bytes = g->z_stride * h;
	if (gtexc) {
		binds[3].phy = g->ctex_phy; binds[3].bytes = g->ctex_stride * g->ctex_h;
	}
	/* Raster grid: tpw 0 = one workgroup per triangle (legacy kernels);
	   the lane-per-triangle kernels cover geom_tpw triangles per workgroup
	   (validated at load to equal the lane count), so ceil(ntris/tpw).  An
	   over-dispatching workgroup is harmless through the kernel's uTriCount
	   guard. */
	tpw = s->geom_tpw ? s->geom_tpw : 1u;
	grid[0] = (ntris + tpw - 1u) / tpw;
	grid[1] = 1u;
	grid[2] = 1u;
	rc = emit_cs_dispatch(rs, binds, 4u, unif + split, num_uniforms - split,
	                      grid, 1);
	if (rc != ADRENO_OK)
		klog("adreno_3d: geom raster rc=%d tris %u tpw=%u\n", rc, ntris, tpw);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Dispatch a loaded program: fullscreen (one invocation per pixel,       */
/* grid = ceil(fb_w/local_x) * ceil(fb_h/local_y) * 1) or the two-pass    */
/* geometry rasterizer when the program carries is_geom.  Returns         */
/* ADRENO_OK, ADRENO_E_DECLINED before anything reached the GPU, or the   */
/* adreno_ring_submit result (TIMEOUT / FAULT / WEDGED).                 */
/* ------------------------------------------------------------------ */
int adreno_3d_draw(uint64_t fb_phy, int32_t fb_w, int32_t fb_h,
                   uint32_t fb_stride, int32_t prog_handle,
                   const adreno_3d_geom_t *geom,
                   const uint32_t *unif, uint32_t num_uniforms) {
	const prog_slot_t *s;
	bind_t binds[1];
	uint32_t grid[3];

	if (!adreno_3d_ready())
		return ADRENO_E_DECLINED;
	if (prog_handle < 0 || prog_handle >= ADRENO_3D_MAX_PROGS)
		return ADRENO_E_DECLINED;
	s = &g_progs[prog_handle];
	if (!s->in_use)
		return ADRENO_E_DECLINED;

	/* --- eligibility: framebuffer GPU-visible, aligned, sane --- */
	if (fb_phy == 0 || fb_w <= 0 || fb_h <= 0)
		return ADRENO_E_DECLINED;
	if (fb_stride < (uint32_t)fb_w * 4u || (fb_stride & 3u) != 0)
		return ADRENO_E_DECLINED;
	if ((fb_phy & 0x3fu) != 0)   /* MEMOBJ base must be 64-aligned */
		return ADRENO_E_DECLINED;
	if ((uint64_t)fb_stride * (uint32_t)fb_h > 0xffffffffu)
		return ADRENO_E_DECLINED;
	if (unif == 0 && num_uniforms > 0)
		return ADRENO_E_DECLINED;

	if (s->is_geom)
		return draw_geom(s, fb_phy, (uint32_t)fb_w, (uint32_t)fb_h, fb_stride,
		                 geom, unif, num_uniforms);

	/* --- fullscreen: only binding 0 (the framebuffer) can be supplied; a
	   kernel declaring any further storage buffer is declined by
	   emit_cs_dispatch's binding check. --- */
	binds[0].phy = fb_phy;
	binds[0].bytes = fb_stride * (uint32_t)fb_h;
	grid[0] = ((uint32_t)fb_w + s->kern[0].k.local_size[0] - 1u) / s->kern[0].k.local_size[0];
	grid[1] = ((uint32_t)fb_h + s->kern[0].k.local_size[1] - 1u) / s->kern[0].k.local_size[1];
	grid[2] = 1u;
	return emit_cs_dispatch(&s->kern[0], binds, 1u, unif, num_uniforms, grid, 0);
}
