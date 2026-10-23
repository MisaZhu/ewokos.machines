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
 *   4. STATIC STATE. Each dispatch emits Mesa's A690 magic_raw table and
 *      SP/TPL1/HLSQ compute context before loading shader state. This avoids
 *      relying on a graphics context inherited from the bootloader. The
 *      resulting compute path still requires hardware validation.
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
#include <pthread.h>
#include "../shaders/blit_blob.h"
#include "../shaders/raster_blob.h"

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

/* Mesa fd6_emit_static_non_context_regs / tu6_init_hw, with the exact
 * FD690 table in vendored freedreno_devices.h (__info18). On A690 the
 * EOLM and linear-mipmap-in-blocks bits are disabled and absent below.
 * Normal UMD state via PKT4; no privileged MMIO or firmware changes. */
static void ib_cs_context(struct ib *b) {
	static const struct { uint32_t reg, value; } state[] = {
		{REG_A6XX_SP_UNKNOWN_AAF2, 0x00c00000},
		{REG_A6XX_PC_MODE_CNTL, 0x1f},
		{REG_A6XX_PC_POWER_CNTL, 7},
		{REG_A6XX_VFD_POWER_CNTL, 7},
		{REG_A6XX_TPL1_DBG_ECO_CNTL, 0x04c00000},
		{REG_A6XX_GRAS_DBG_ECO_CNTL, 0},
		{REG_A6XX_SP_CHICKEN_BITS, 0x00001400},
		{REG_A6XX_SP_DBG_ECO_CNTL, 0x01200000},
		{REG_A6XX_HLSQ_DBG_ECO_CNTL, 0},
		{REG_A6XX_VPC_DBG_ECO_CNTL, 0x02000400},
		{REG_A6XX_UCHE_UNKNOWN_0E12, 1},
		{REG_A6XX_SP_NC_MODE_CNTL_2, 0},
		{REG_A6XX_SP_PERFCTR_SHADER_MASK, 0x3f},
		{REG_A6XX_TPL1_UNKNOWN_B605, 0x44},
		{REG_A6XX_HLSQ_UNKNOWN_BE00, 0x80},
		{REG_A6XX_HLSQ_UNKNOWN_BE01, 0},
		{REG_A6XX_HLSQ_SHARED_CONSTS, 0},
		{REG_A6XX_SP_GFX_USIZE, 0},
		{REG_A6XX_SP_UNKNOWN_A9A8, 0},
		/* fd6_program emit_shader_regs: constant demotion, ISAMMODE_GL,
		 * no shared constants. XML: bit0 | (2 << 1). */
		{REG_A6XX_SP_MODE_CNTL, 0x5},
		/* GL | CLAMP_ROUND_TRUNCATE << 5 | datatype override.
		 * A6XX has no CLAMP_DISABLE bit (that field is A8XX-only). */
		{REG_A6XX_TPL1_MODE_CNTL, 0xa2},
	};
	ib_pkt7(b, CP_WAIT_FOR_IDLE, 0);
	ib_pkt7(b, CP_EVENT_WRITE, 1);
	ib_w(b, CACHE_INVALIDATE);
	for (uint32_t i = 0; i < sizeof(state) / sizeof(state[0]); i++) {
		ib_pkt4(b, state[i].reg, 1);
		ib_w(b, state[i].value);
	}
	/* No retained draw groups: all compute state below is immediate. */
	ib_pkt7(b, CP_SET_DRAW_STATE, 3);
	ib_w(b, 1u << 18); /* DISABLE_ALL_GROUPS */
	ib_w(b, 0);
	ib_w(b, 0);
	ib_pkt7(b, CP_SET_MODE, 1);
	ib_w(b, 1);
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
	uint8_t      pixel_owned;
	uint16_t     geom_tpw;
	uint32_t     cs2_uniform_blob_dwords;
	kern_slot_t  kern[2];     /* [0] main / raster, [1] xform (geom only) */
} prog_slot_t;

static prog_slot_t g_progs[ADRENO_3D_MAX_PROGS];
/* IPC workers may draw/upload concurrently. Protect program lifetime and the
   shared vertex output across BOTH submissions, not only each ring submit. */
static pthread_mutex_t g_3d_lock = PTHREAD_MUTEX_INITIALIZER;

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
	if ((uint64_t)k->local_size[0] * k->local_size[1] * k->local_size[2] > 1024u ||
	    k->shared_size > 32768u || k->uniform_blob_dwords > ADRENO_3D_IB_MAX)
		return -1;
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
		if (total == 0 || total > 0xffffffffu) {
			klog("adreno_3d: pvtmem total %llu B out of u32 range "
			     "(pvtmem_size %u/fiber per_fiber %u per_sp %u)\n",
			     (unsigned long long)total, k->pvtmem_size,
			     ks->per_fiber_size, ks->per_sp_size);
			goto fail;
		}
		if (adreno_bo_alloc(&ks->pvt_bo, (uint32_t)total) != 0) {
			/* A spilling kernel needs ONE contiguous dma block of
			   per_fiber * fibers_per_sp(4096) * num_sp_cores(8) ==
			   per_fiber * 32768 bytes (512 B/fiber => 16 MB, 4 KB =>
			   128 MB, the 16 KB cap => 512 MB).  dma_alloc is a
			   first-fit slice of the kernel dma pool (kernel.conf
			   dma_size), so a pool smaller than `total` fails HERE.
			   This is exactly the silent failure that dropped the
			   ocean kernel to the (un-scaled, ~0.1 FPS) CPU path and
			   looked like a hang; log it loudly so a too-small pool
			   is diagnosable instead of invisible. */
			klog("adreno_3d: pvtmem BO alloc FAILED for total %llu B "
			     "(pvtmem_size %u/fiber per_fiber %u per_sp %u) - "
			     "kernel dma_size pool too small for this kernel\n",
			     (unsigned long long)total, k->pvtmem_size,
			     ks->per_fiber_size, ks->per_sp_size);
			goto fail;
		}
		ks->pvt_bo.iova = ks->pvt_bo.pa;
		if ((ks->pvt_bo.pa & 0x1fu) != 0) {   /* tu6_emit_xs assert */
			klog("adreno_3d: pvtmem BO pa %llx not 32-aligned\n",
			     (unsigned long long)ks->pvt_bo.pa);
			goto fail;
		}
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
static int program_load_locked(const void *blob, uint32_t size) {
	const adreno_prog_blob_t *h;
	const uint8_t *payload;
	prog_slot_t *s = 0;
	uint64_t total;
	int i, slot = -1, have_xform;

	if (!adreno_3d_ready())
		return -1;
	if (adreno_wedged())
		return ADRENO_E_WEDGED;
	if (blob == 0 || size < sizeof(adreno_prog_blob_t))
		return -1;

	h = (const adreno_prog_blob_t *)blob;
	if (h->magic != ADRENO_PROG_MAGIC ||
	    (h->version != ADRENO_PROG_VERSION && h->version != ADRENO_PROG_PIXEL_VERSION))
		return -1;   /* not an Adreno blob (e.g. V3D-QPU) */

	/* --- header geometry.  kern[0] is mandatory; kern[1] (xform) must be
	   present exactly when the program is a two-pass geometry program. --- */
	if (kern_validate(&h->kern[0]) != 0)
		return -1;
	have_xform = (h->kern[1].binary_words != 0);
	if (h->is_geom > 4u)
		return -1;
	if (h->reserved || (h->version == ADRENO_PROG_PIXEL_VERSION &&
	    h->is_geom != 1u && h->is_geom != 2u))
		return -1;
	if (h->is_geom != 0 && !have_xform)
		return -1;
	if (h->is_geom == 0 && have_xform)
		return -1;
	if (have_xform && kern_validate(&h->kern[1]) != 0)
		return -1;
	if (have_xform && h->cs2_uniform_blob_dwords != h->kern[1].uniform_blob_dwords)
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
	s->pixel_owned = h->version == ADRENO_PROG_PIXEL_VERSION;
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
static int program_free_locked(int32_t handle) {
	prog_slot_t *s;
	if (handle < 0 || handle >= ADRENO_3D_MAX_PROGS)
		return -1;
	s = &g_progs[handle];
	if (!s->in_use)
		return 0;
	/* A failed dispatch may still fetch its shader/private storage. */
	if (adreno_wedged())
		return ADRENO_E_WEDGED;
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
/* Build one 16-dword a6xx sampled-texture descriptor (the                */
/* DESC_SINGLE_PLANE variant of A6XX_TEX_MEMOBJ) for a LINEAR RGBA8 image,  */
/* mirroring fdl6_texture(TILE6_LINEAR, FMT6_8_8_8_8_UNORM).  `base` must be  */
/* 32-byte aligned (BASE_LO is shr5); `stride`/`array_pitch` are in bytes     */
/* (array_pitch is the per-layer size, internally shr12 by the macro).  A     */
/* cube sets TYPE=A6XX_TEX_CUBE so the 6 faces are strided by array_pitch;    */
/* DEPTH stays 1 (a single image / single cubemap).  Colour order follows the */
/* same SWAP=WZYX + identity SWIZ convention the framebuffer/blit path uses;  */
/* if the device shows red/blue swapped this is the single knob to flip.      */
/* WIDTH/HEIGHT/DEPTH hold the RAW size (w, h, 1), not size-1, per            */
/* a6xx_descriptors.xml + mesa fd6_emit_textures; a single 2D image and a     */
/* single cubemap both use DEPTH=1 (TYPE=A6XX_TEX_CUBE implies the 6 faces,   */
/* strided by array_pitch).  Only the colour SWAP/SWIZ above is a field the   */
/* device bring-up may still need to flip (QEMU models no Adreno).            */
/* ------------------------------------------------------------------ */
static void build_tex_desc(uint32_t *desc, uint64_t base, uint32_t w, uint32_t h,
                           uint32_t stride, uint32_t array_pitch, int cube) {
	memset(desc, 0, A6XX_TEX_CONST_DWORDS * 4u);
	desc[0] = A6XX_TEX_MEMOBJ_0_TILE_MODE(TILE6_LINEAR) |
	          A6XX_TEX_MEMOBJ_0_SWAP(A3XX_COLOR_SWAP_WZYX) |
	          A6XX_TEX_MEMOBJ_0_FMT(FMT6_8_8_8_8_UNORM) |
	          A6XX_TEX_MEMOBJ_0_MIPLVLS(0) |
	          A6XX_TEX_MEMOBJ_0_SWIZ_X(A6XX_TEX_SWIZ_X) |
	          A6XX_TEX_MEMOBJ_0_SWIZ_Y(A6XX_TEX_SWIZ_Y) |
	          A6XX_TEX_MEMOBJ_0_SWIZ_Z(A6XX_TEX_SWIZ_Z) |
	          A6XX_TEX_MEMOBJ_0_SWIZ_W(A6XX_TEX_SWIZ_W);
	desc[1] = A6XX_TEX_MEMOBJ_1_WIDTH(w & 0x7fffu) |
	          A6XX_TEX_MEMOBJ_1_HEIGHT(h & 0x7fffu);
	desc[2] = A6XX_TEX_MEMOBJ_2_PITCHALIGN(0) |
	          A6XX_TEX_MEMOBJ_2_PITCH(stride & 0x3fffffu) |
	          A6XX_TEX_MEMOBJ_2_TYPE(cube ? A6XX_TEX_CUBE : A6XX_TEX_2D);
	desc[3] = A6XX_TEX_MEMOBJ_3_ARRAY_PITCH(array_pitch);
	desc[4] = A6XX_TEX_MEMOBJ_4_BASE_LO(base);
	desc[5] = A6XX_TEX_MEMOBJ_5_BASE_HI((uint32_t)(base >> 32)) |
	          A6XX_TEX_MEMOBJ_5_DEPTH(1);
	/* desc[6..15] stay 0 (single plane, one mip level, no flag buffer). */
}

/* ------------------------------------------------------------------ */
/* Build one 4-dword a6xx_tex_samp sampler descriptor.  `linear` picks        */
/* NEAREST vs LINEAR magnification / minification (from the ctex ub_pad high  */
/* nibble, G2D_PGL_CTEX_FILTER_*); wrap is REPEAT on all axes (the client does */
/* not convey a wrap mode).  Coords stay normalized, LOD pinned to level 0    */
/* (the uploaded image has a single mip level).  dword2/dword3 stay 0.        */
/* ------------------------------------------------------------------ */
static void build_samp_desc(uint32_t *desc, int linear) {
	uint32_t f = linear ? A6XX_TEX_LINEAR : A6XX_TEX_NEAREST;
	memset(desc, 0, A6XX_TEX_SAMP_DWORDS * 4u);
	desc[0] = A6XX_TEX_SAMP_0_XY_MAG(f) |
	          A6XX_TEX_SAMP_0_XY_MIN(f) |
	          A6XX_TEX_SAMP_0_WRAP_S(A6XX_TEX_REPEAT) |
	          A6XX_TEX_SAMP_0_WRAP_T(A6XX_TEX_REPEAT) |
	          A6XX_TEX_SAMP_0_WRAP_R(A6XX_TEX_REPEAT);
	desc[1] = A6XX_TEX_SAMP_1_MAX_LOD(0) |
	          A6XX_TEX_SAMP_1_MIN_LOD(0);
	/* desc[2], desc[3] stay 0 (no reduction / border colour / aniso). */
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

/* Per-dispatch BO layout: [UAV descriptors: num_ssbo x 64 B][texture
   descriptors: num_samp x 64 B, 64-aligned (SP_CS_TEXMEMOBJ_BASE)][sampler
   descriptors: num_samp x 16 B (SP_CS_SAMPLER_BASE, 16-aligned)][dummy texel
   scratch: 512 B zeroed, for unbound slots][UBO-0 copy of the kernel's uniform
   region, 64-byte aligned, >= 64 B].  With num_samp == 0 the texture / sampler
   / dummy regions are all empty and the layout is byte-identical to before. */
#define DESC_UBO_MIN_BYTES  64u
#define DESC_DUMMY_TEX_BYTES 512u   /* 6 cube faces x 64 B, zeroed -> samples 0 */

/* Emit and submit one compute dispatch of kernel `ks` over grid[3]
   workgroups, binding the SSBO slots from `binds` (indexed by declared
   binding), the sampled-texture units from `tex[num_tex]` (indexed by ctex
   unit == texture_index; a slot the kernel reads but the request left unbound
   samples a zeroed 1x1 dummy), and feeding `unif[num_uniforms]` as UBO 0.
   Every dispatch drains and invalidates UCHE before loading state. Returns
   ADRENO_OK, ADRENO_E_DECLINED before anything reached the GPU, or the
   adreno_ring_submit result (TIMEOUT / FAULT / WEDGED). */
static int emit_cs_dispatch(const kern_slot_t *ks, const bind_t *binds,
                            uint32_t nbinds, const adreno_3d_tex_t *tex,
                            uint32_t num_tex, const uint32_t *unif,
                            uint32_t num_uniforms, const uint32_t grid[3]) {
	const adreno_prog_kernel_t *k = &ks->k;
	struct ib b;
	adreno_bo_t desc_bo;
	uint32_t bin_iova_lo, bin_iova_hi, pvt_lo, pvt_hi;
	uint64_t bin_iova, pvt_iova, ubo_iova, tex_iova, samp_iova, dummy_iova;
	uint32_t thrsz, thrsz_cs, shared_size, cram, preload;
	uint32_t gsx, gsy, gsz;
	uint32_t desc_bytes, ubo_off, ubo_bytes, ubo_vec4s, ubo_region_dwords;
	uint32_t num_samp, tex_off, samp_off, dummy_off;
	uint32_t i;
	int ret;

	if (unif == 0 && num_uniforms > 0)
		return ADRENO_E_DECLINED;
	if (num_uniforms > ADRENO_3D_IB_MAX)   /* can never fit the IB anyway */
		return ADRENO_E_DECLINED;
	if (grid[0] == 0 || grid[1] == 0 || grid[2] == 0)
		return ADRENO_E_DECLINED;

	/* --- sampled-texture eligibility: the kernel reads slots [0,num_samp);
	   a bound slot must be a linear RGBA8 image the descriptor can address
	   (32-byte-aligned base for the shr5 BASE_LO, in-range dims/stride).  An
	   unbound slot is fine: it samples the zeroed dummy.  A genuinely
	   unsupported bound slot declines to the CPU rather than risk a fault. --- */
	num_samp = k->num_samp;
	if (num_samp > ADRENO_PROG_MAX_TEX)
		return ADRENO_E_DECLINED;
	for (i = 0; i < num_samp; i++) {
		const adreno_3d_tex_t *t;
		if (i >= num_tex || tex[i].phy == 0)
			continue;                    /* unbound -> dummy below */
		t = &tex[i];
		if (t->tiling != 0)                  /* q8b only uploads linear */
			return ADRENO_E_DECLINED;
		if ((t->phy & 0x1fu) != 0)           /* BASE_LO is shr5 */
			return ADRENO_E_DECLINED;
		if (t->w == 0 || t->h == 0 || t->w > 0x7fffu || t->h > 0x7fffu)
			return ADRENO_E_DECLINED;
		if (t->stride == 0 || t->stride > 0x3fffffu)
			return ADRENO_E_DECLINED;
		if ((uint64_t)t->w * 4u > t->stride)    /* row must fit the pitch */
			return ADRENO_E_DECLINED;
		if (k->tex_type[i] == ADRENO_PROG_TEX_CUBE) {
			/* A samplerCube slot must be fed a real cubemap (>= 6 layers at a
			   4K-aligned per-layer stride): otherwise the 6 face reads stride
			   past a single 2D allocation (out-of-bounds into neighbouring RAM,
			   and past SMMU CB0's map a fault), or land misaligned (garbage).
			   A fault here wedges the device permanently, so decline. */
			uint64_t layer = (uint64_t)t->arrstride64 * 64u;
			if (t->depth < 6u || t->arrstride64 == 0u ||
			    (layer & 0xfffu) != 0 || layer > 0x7ffff000ull)
				return ADRENO_E_DECLINED;   /* ARRAY_PITCH is a 23-bit shr12 field */
		}
	}

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

	/* --- per-dispatch BO: UAV descriptors + texture/sampler descriptors +
	   the zeroed dummy texel scratch + the UBO-0 uniform copy --- */
	ubo_region_dwords = num_uniforms < k->uniform_blob_dwords ? num_uniforms
	                                                          : k->uniform_blob_dwords;
	desc_bytes = k->num_ssbo * A6XX_TEX_CONST_DWORDS * 4u;
	tex_off   = align_up(desc_bytes, 64u);                       /* TEXMEMOBJ_BASE: 64-aligned */
	samp_off  = tex_off + num_samp * A6XX_TEX_CONST_DWORDS * 4u; /* SAMPLER_BASE: 16-aligned   */
	dummy_off = align_up(samp_off + num_samp * A6XX_TEX_SAMP_DWORDS * 4u, 64u);
	ubo_off   = dummy_off + (num_samp ? DESC_DUMMY_TEX_BYTES : 0u);
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
	/* Texture + sampler descriptors, one pair per slot the kernel reads.
	   An unbound slot points at the zeroed dummy scratch so it samples
	   (0,0,0,0) instead of faulting on a null base. */
	if (num_samp > 0) {
		uint8_t *tex_base  = (uint8_t *)desc_bo.va + tex_off;
		uint8_t *samp_base = (uint8_t *)desc_bo.va + samp_off;
		dummy_iova = desc_bo.iova + dummy_off;
		memset((uint8_t *)desc_bo.va + dummy_off, 0, DESC_DUMMY_TEX_BYTES);
		tex_iova  = desc_bo.iova + tex_off;
		samp_iova = desc_bo.iova + samp_off;
		for (i = 0; i < num_samp; i++) {
			uint32_t *td = (uint32_t *)(tex_base + i * A6XX_TEX_CONST_DWORDS * 4u);
			uint32_t *sd = (uint32_t *)(samp_base + i * A6XX_TEX_SAMP_DWORDS * 4u);
			int cube = (i < ADRENO_PROG_MAX_TEX &&
			            k->tex_type[i] == ADRENO_PROG_TEX_CUBE);
			int linear = 0;
			if (i < num_tex && tex[i].phy != 0) {
				const adreno_3d_tex_t *t = &tex[i];
				uint32_t stride = t->stride;   /* non-zero: validated above */
				/* cube: per-layer bytes from arrstride64 (validated: non-zero,
				   4K-aligned, fits the 23-bit shr12 ARRAY_PITCH field); 2D: the
				   single layer's size (ARRAY_PITCH is unused for one layer). */
				uint32_t ap = cube ? (uint32_t)((uint64_t)t->arrstride64 * 64u)
				                   : stride * t->h;
				build_tex_desc(td, t->phy, t->w, t->h, stride, ap, cube);
				linear = ((t->ub_pad >> 4) & 0xFu) == 1u;  /* G2D_PGL_CTEX_FILTER_LINEAR */
			} else {
				/* dummy: 1x1 (cube faces strided 64 B, all inside the scratch) */
				build_tex_desc(td, dummy_iova, 1u, 1u, 4u, cube ? 64u : 4u, cube);
			}
			build_samp_desc(sd, linear);
		}
		/* Device bring-up diagnostic: dump the first few textured
		   bindings so a black-screen report can be matched against what
		   the daemon actually bound (dummy vs real, cube geometry). */
		{
			static int g_tex_log = 3;
			if (g_tex_log > 0) {
				g_tex_log--;
				for (i = 0; i < num_samp; i++) {
					const adreno_3d_tex_t *t = (i < num_tex) ? &tex[i] : 0;
					slog("adreno_3d: tex slot %u %s phy=%llx %ux%u stride=%u depth=%u arr64=%u tiling=%u ub=%x\n",
					     i, (i < ADRENO_PROG_MAX_TEX &&
					         k->tex_type[i] == ADRENO_PROG_TEX_CUBE) ? "cube" : "2d",
					     (t && t->phy) ? (unsigned long long)t->phy : 0ull,
					     t ? t->w : 0u, t ? t->h : 0u, t ? t->stride : 0u,
					     t ? (uint32_t)t->depth : 0u, t ? t->arrstride64 : 0u,
					     t ? (uint32_t)t->tiling : 0u, t ? (uint32_t)t->ub_pad : 0u);
				}
			}
		}
	} else {
		tex_iova = samp_iova = dummy_iova = 0ull;
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

	/* Previous jobs retired through CACHE_FLUSH_TS. Invalidate before
	   fetching this shader, descriptors, uniforms or a previous pass's output. */
	ib_cs_context(&b);

	/* === dispatch prologue: enter the COMPUTE render mode === */
	ib_pkt7(&b, CP_SET_MARKER, 1);
	ib_w(&b, A6XX_SET_MARKER_COMPUTE);

	/* === 1. SP_UPDATE_CNTL(cs_state | cs_uav) === */
	ib_pkt4(&b, REG_A6XX_SP_UPDATE_CNTL, 1);
	ib_w(&b, A6XX_SP_UPDATE_CNTL_CS_STATE | A6XX_SP_UPDATE_CNTL_CS_UAV |
	         A6XX_SP_UPDATE_CNTL_CS_SHARED_CONST);

	/* === 2. SP_CS_CONST_CONFIG(constlen,enabled) + SP_CS_CONFIG === */
	ib_pkt4(&b, REG_A6XX_SP_CS_CONST_CONFIG, 1);
	ib_w(&b, A6XX_SP_CS_CONST_CONFIG_CONSTLEN(k->constlen) |
	         A6XX_SP_CS_CONST_CONFIG_ENABLED);
	ib_pkt4(&b, REG_A6XX_SP_CS_CONFIG, 1);
	ib_w(&b, A6XX_SP_CS_CONFIG_ENABLED |
	         A6XX_SP_CS_CONFIG_NTEX(num_samp) |
	         A6XX_SP_CS_CONFIG_NSAMP(num_samp) |
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

	/* === 9b. sampled textures + samplers (fd6_emit_textures, bindful):
	   sampler descriptors are preloaded ST6_SHADER / SB6_CS_TEX then pointed
	   to by SP_CS_SAMPLER_BASE; texture descriptors ST6_CONSTANTS /
	   SB6_CS_TEX then SP_CS_TEXMEMOBJ_BASE.  NUM_UNIT is the descriptor
	   COUNT (not dwords); the counts already went to SP_CS_CONFIG.NTEX/.NSAMP
	   in step 2. === */
	if (num_samp > 0) {
		ib_pkt7(&b, CP_LOAD_STATE6_FRAG, 3);
		ib_w(&b, A6XX_CP_LOAD_STATE6_0_DST_OFF(0) |
		         A6XX_CP_LOAD_STATE6_0_STATE_TYPE(A6XX_ST6_SHADER) |
		         A6XX_CP_LOAD_STATE6_0_STATE_SRC(A6XX_SS6_INDIRECT) |
		         A6XX_CP_LOAD_STATE6_0_STATE_BLOCK(A6XX_SB6_CS_TEX) |
		         A6XX_CP_LOAD_STATE6_0_NUM_UNIT(num_samp));
		ib_w(&b, (uint32_t)(samp_iova & 0xffffffffu));
		ib_w(&b, (uint32_t)(samp_iova >> 32));
		ib_pkt4(&b, REG_A6XX_SP_CS_SAMPLER_BASE_LO, 2);
		ib_w(&b, (uint32_t)(samp_iova & 0xffffffffu));
		ib_w(&b, (uint32_t)(samp_iova >> 32));

		ib_pkt7(&b, CP_LOAD_STATE6_FRAG, 3);
		ib_w(&b, A6XX_CP_LOAD_STATE6_0_DST_OFF(0) |
		         A6XX_CP_LOAD_STATE6_0_STATE_TYPE(A6XX_ST6_CONSTANTS) |
		         A6XX_CP_LOAD_STATE6_0_STATE_SRC(A6XX_SS6_INDIRECT) |
		         A6XX_CP_LOAD_STATE6_0_STATE_BLOCK(A6XX_SB6_CS_TEX) |
		         A6XX_CP_LOAD_STATE6_0_NUM_UNIT(num_samp));
		ib_w(&b, (uint32_t)(tex_iova & 0xffffffffu));
		ib_w(&b, (uint32_t)(tex_iova >> 32));
		ib_pkt4(&b, REG_A6XX_SP_CS_TEXMEMOBJ_BASE, 2);
		ib_w(&b, (uint32_t)(tex_iova & 0xffffffffu));
		ib_w(&b, (uint32_t)(tex_iova >> 32));
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
/* Exact nearest copy / straight-alpha source-over. This private program does
   not consume any application handle. blit_blob.h comes from shaders/blit.comp
   through the same GLSL/NIR/ir3 compiler as application programs. */
static int compute_blit(uint64_t src_phy, int32_t src_w, int32_t src_h,
                         int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                         uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                         int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                         int endpoints, int alpha) {
	static kern_slot_t kernel;
	static int loaded;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	bind_t binds[2];
	uint32_t params[12], grid[3], shift = endpoints ? 15u : 0u;
	uint32_t step_x, step_y;
	int rc;
	if (!adreno_3d_ready()) return ADRENO_E_DECLINED;
	if (adreno_wedged()) return ADRENO_E_WEDGED;
	if (alpha < -1 || alpha > 255 || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
	    src_w > 16384 || src_h > 16384 || dst_w > 16384 || dst_h > 16384 ||
	    sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
	    sx < 0 || sy < 0 || dx < 0 || dy < 0 ||
	    sw > src_w || sh > src_h || dw > dst_w || dh > dst_h ||
	    sx > src_w-sw || sy > src_h-sh || dx > dst_w-dw || dy > dst_h-dh ||
	    !src_phy || !dst_phy || (src_phy & 63u) || (dst_phy & 63u))
		return ADRENO_E_DECLINED;
	binds[0].phy = src_phy; binds[0].bytes = (uint32_t)src_w*src_h*4u;
	binds[1].phy = dst_phy; binds[1].bytes = (uint32_t)dst_w*dst_h*4u;
	/* In-place resizing needs an intermediate; never let invocations race. */
	if (src_phy < dst_phy + binds[1].bytes && dst_phy < src_phy + binds[0].bytes)
		return ADRENO_E_DECLINED;
	if (endpoints) {
		step_x = dw <= 1 ? 0 : (((uint32_t)(sw-1)<<15)+(uint32_t)dw-2u)/(uint32_t)(dw-1);
		step_y = dh <= 1 ? 0 : (((uint32_t)(sh-1)<<15)+(uint32_t)dh-2u)/(uint32_t)(dh-1);
	} else {
		step_x = sw;
		step_y = sh;
	}
	params[0]=src_w; params[1]=dst_w; params[2]=sx; params[3]=sy;
	params[4]=dx; params[5]=dy; params[6]=dw; params[7]=dh;
	params[8]=step_x; params[9]=step_y; params[10]=shift; params[11]=0;
	if (alpha >= 0) { params[10]=alpha; params[11]=1; }
	grid[0]=((uint32_t)dw+7u)/8u; grid[1]=((uint32_t)dh+7u)/8u; grid[2]=1;
	pthread_mutex_lock(&lock);
	if (!loaded) {
		const adreno_prog_blob_t *h = (const void *)q8b_2d_blob;
		if (h->magic != ADRENO_PROG_MAGIC || h->version != ADRENO_PROG_VERSION ||
		    q8b_2d_blob_len != sizeof(*h)+h->payload_bytes ||
		    kern_validate(&h->kern[0]) || h->kern[0].uniform_blob_dwords != 12 ||
		    h->kern[0].num_ssbo != 2 || h->kern[0].pvtmem_size != 0 ||
		    h->kern[0].local_size[0] != 8 || h->kern[0].local_size[1] != 8 ||
		    h->kern[0].local_size[2] != 1 ||
		    kern_load(&kernel,&h->kern[0],q8b_2d_blob+sizeof(*h))) {
			pthread_mutex_unlock(&lock);
			return ADRENO_E_DECLINED;
		}
		loaded=1;
	}
	rc=emit_cs_dispatch(&kernel,binds,2,NULL,0,params,12,grid);
	pthread_mutex_unlock(&lock);
	return rc;
}

int adreno_compute_scale(uint64_t src_phy, int32_t src_w, int32_t src_h,
                         int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                         uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                         int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                         int endpoints) {
	return compute_blit(src_phy,src_w,src_h,sx,sy,sw,sh,
		dst_phy,dst_w,dst_h,dx,dy,dw,dh,endpoints,-1);
}

int adreno_compute_blend(uint64_t src_phy, int32_t src_w, int32_t src_h,
                         int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                         uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                         int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                         uint8_t alpha) {
	return compute_blit(src_phy,src_w,src_h,sx,sy,sw,sh,
		dst_phy,dst_w,dst_h,dx,dy,dw,dh,0,alpha);
}

/* Solid source-over and inverse-mapped rotation use a separate ordinary
   application kernel, preserving the validated scale/blend kernel. */
static int compute_raster(uint64_t src_phy, int32_t src_w, int32_t src_h,
                          uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                          int32_t x, int32_t y, int32_t w, int32_t h,
                          int32_t mode, uint32_t value, int32_t c, int32_t s) {
	static kern_slot_t kernel;
	static int loaded;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	bind_t binds[2];
	uint32_t params[12], grid[3];
	int rc;
	if (!adreno_3d_ready()) return ADRENO_E_DECLINED;
	if (adreno_wedged()) return ADRENO_E_WEDGED;
	if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
	    src_w > 16384 || src_h > 16384 || dst_w > 16384 || dst_h > 16384 ||
	    x < 0 || y < 0 || w <= 0 || h <= 0 || w > dst_w || h > dst_h ||
	    x > dst_w-w || y > dst_h-h ||
	    !src_phy || !dst_phy || (src_phy & 63u) || (dst_phy & 63u) ||
	    (mode != 0 && mode != 1) || c < -16384 || c > 16384 || s < -16384 || s > 16384)
		return ADRENO_E_DECLINED;
	binds[0].phy=src_phy; binds[0].bytes=(uint32_t)src_w*src_h*4u;
	binds[1].phy=dst_phy; binds[1].bytes=(uint32_t)dst_w*dst_h*4u;
	if (src_phy+binds[0].bytes < src_phy || dst_phy+binds[1].bytes < dst_phy)
		return ADRENO_E_DECLINED;
	/* Solid fill reads only its own destination pixel. Rotating overlapping
	   allocations would race other invocations and must decline beforehand. */
	if (mode == 1) {
		int32_t rw, rh;
		if (value >= 360 || x || y || w != dst_w || h != dst_h)
			return ADRENO_E_DECLINED;
		if (value % 90 == 0) {
			rw=(value == 90 || value == 270)?src_h:src_w;
			rh=(value == 90 || value == 270)?src_w:src_h;
		} else {
			uint32_t ac=c<0?-c:c, as=s<0?-s:s;
			rw=((uint32_t)src_w*ac+(uint32_t)src_h*as+16383u)>>14;
			rh=((uint32_t)src_w*as+(uint32_t)src_h*ac+16383u)>>14;
		}
		if (dst_w != rw || dst_h != rh ||
		    (src_phy < dst_phy+binds[1].bytes && dst_phy < src_phy+binds[0].bytes))
			return ADRENO_E_DECLINED;
	}
	params[0]=src_w; params[1]=dst_w; params[2]=src_w; params[3]=src_h;
	params[4]=x; params[5]=y; params[6]=w; params[7]=h;
	params[8]=mode; params[9]=value; params[10]=(uint32_t)c; params[11]=(uint32_t)s;
	grid[0]=((uint32_t)w+7u)/8u; grid[1]=((uint32_t)h+7u)/8u; grid[2]=1;
	pthread_mutex_lock(&lock);
	if (!loaded) {
		const adreno_prog_blob_t *hdr=(const void *)q8b_raster_blob;
		if (hdr->magic != ADRENO_PROG_MAGIC || hdr->version != ADRENO_PROG_VERSION ||
		    q8b_raster_blob_len != sizeof(*hdr)+hdr->payload_bytes ||
		    kern_validate(&hdr->kern[0]) || hdr->kern[0].uniform_blob_dwords != 12 ||
		    hdr->kern[0].num_ssbo != 2 || hdr->kern[0].pvtmem_size != 0 ||
		    hdr->kern[0].local_size[0] != 8 || hdr->kern[0].local_size[1] != 8 ||
		    hdr->kern[0].local_size[2] != 1 ||
		    kern_load(&kernel,&hdr->kern[0],q8b_raster_blob+sizeof(*hdr))) {
			pthread_mutex_unlock(&lock);
			return ADRENO_E_DECLINED;
		}
		loaded=1;
	}
	rc=emit_cs_dispatch(&kernel,binds,2,NULL,0,params,12,grid);
	pthread_mutex_unlock(&lock);
	return rc;
}

int adreno_compute_fill_alpha(uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                             int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t color) {
	return compute_raster(dst_phy,dst_w,dst_h,dst_phy,dst_w,dst_h,
		x,y,w,h,0,color,0,0);
}

int adreno_compute_rotate(uint64_t src_phy, int32_t src_w, int32_t src_h,
                          uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                          int32_t degree, int32_t cos_q14, int32_t sin_q14) {
	return compute_raster(src_phy,src_w,src_h,dst_phy,dst_w,dst_h,
		0,0,dst_w,dst_h,1,(uint32_t)degree,cos_q14,sin_q14);
}

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
	     (uint64_t)g->ctex_w * 4u > g->ctex_stride ||
	     (uint64_t)g->ctex_stride * g->ctex_h > 0xffffffffu ||
	     (g->ctex_stride & 3u) != 0))
		return ADRENO_E_DECLINED;
	/* Texture shaders use ty*uTexW+tx; they do not carry a separate pitch. */
	if (gtexc && (uint64_t)g->ctex_w * 4u != g->ctex_stride)
		return ADRENO_E_DECLINED;
	if (g->z_phy == 0 || (g->z_phy & 0x3fu) != 0)
		return ADRENO_E_DECLINED;
	/* Shared raster shaders index colour and depth with uFbStride. */
	if (g->z_stride != fb_stride)
		return ADRENO_E_DECLINED;
	if (rs->k.local_size[2] != 1u || xf->k.local_size[2] != 1u)
		return ADRENO_E_DECLINED;
	if (split > num_uniforms || num_uniforms - split < rs->k.uniform_blob_dwords ||
	    num_uniforms - split > ADRENO_3D_IB_MAX || !unif)
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
	rc = emit_cs_dispatch(xf, binds, 4u, NULL, 0, unif, split, grid);
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
	if (s->pixel_owned) {
		grid[0] = (w + rs->k.local_size[0] - 1u) / rs->k.local_size[0];
		grid[1] = (h + rs->k.local_size[1] - 1u) / rs->k.local_size[1];
	}
	rc = emit_cs_dispatch(rs, binds, 4u, NULL, 0, unif + split, num_uniforms - split,
	                      grid);
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
static int draw_locked(uint64_t fb_phy, int32_t fb_w, int32_t fb_h,
                   uint32_t fb_stride, int32_t prog_handle,
                   const adreno_3d_geom_t *geom,
                   const adreno_3d_tex_t *tex, uint32_t num_tex,
                   const uint32_t *unif, uint32_t num_uniforms) {
	const prog_slot_t *s;
	bind_t binds[1];
	uint32_t grid[3];

	if (!adreno_3d_ready())
		return ADRENO_E_DECLINED;
	if (adreno_wedged())
		return ADRENO_E_WEDGED;
	if (prog_handle < 0 || prog_handle >= ADRENO_3D_MAX_PROGS)
		return ADRENO_E_DECLINED;
	s = &g_progs[prog_handle];
	if (!s->in_use)
		return ADRENO_E_DECLINED;

	/* --- eligibility: framebuffer GPU-visible, aligned, sane --- */
	if (fb_phy == 0 || fb_w <= 0 || fb_h <= 0 || (uint64_t)fb_w * 4u > fb_stride)
		return ADRENO_E_DECLINED;
	if (fb_stride < (uint32_t)fb_w * 4u || (fb_stride & 3u) != 0)
		return ADRENO_E_DECLINED;
	if ((fb_phy & 0x3fu) != 0)   /* MEMOBJ base must be 64-aligned */
		return ADRENO_E_DECLINED;
	if ((uint64_t)fb_stride * (uint32_t)fb_h > 0xffffffffu)
		return ADRENO_E_DECLINED;
	if (unif == 0 && num_uniforms > 0)
		return ADRENO_E_DECLINED;
	if (tex == 0 && num_tex > 0)
		return ADRENO_E_DECLINED;

	if (s->is_geom)
		return draw_geom(s, fb_phy, (uint32_t)fb_w, (uint32_t)fb_h, fb_stride,
		                 geom, unif, num_uniforms);

	/* --- fullscreen: binding 0 (the framebuffer) is the only SSBO that can
	   be supplied; a kernel declaring any further storage buffer is declined
	   by emit_cs_dispatch's binding check.  Sampled textures ride `tex`
	   (ctex unit i -> texture_index i); a slot the kernel reads but the
	   request left unbound samples a zeroed dummy. --- */
	binds[0].phy = fb_phy;
	binds[0].bytes = fb_stride * (uint32_t)fb_h;
	grid[0] = ((uint32_t)fb_w + s->kern[0].k.local_size[0] - 1u) / s->kern[0].k.local_size[0];
	grid[1] = ((uint32_t)fb_h + s->kern[0].k.local_size[1] - 1u) / s->kern[0].k.local_size[1];
	grid[2] = 1u;
	return emit_cs_dispatch(&s->kern[0], binds, 1u, tex, num_tex, unif, num_uniforms, grid);
}

int adreno_3d_program_load(const void *blob, uint32_t size) {
	pthread_mutex_lock(&g_3d_lock);
	int rc = program_load_locked(blob, size);
	pthread_mutex_unlock(&g_3d_lock);
	return rc;
}

int adreno_3d_program_free(int32_t handle) {
	pthread_mutex_lock(&g_3d_lock);
	int rc = program_free_locked(handle);
	pthread_mutex_unlock(&g_3d_lock);
	return rc;
}

int adreno_3d_draw(uint64_t fb_phy, int32_t fb_w, int32_t fb_h,
                   uint32_t fb_stride, int32_t prog_handle,
                   const adreno_3d_geom_t *geom,
                   const adreno_3d_tex_t *tex, uint32_t num_tex,
                   const uint32_t *unif, uint32_t num_uniforms) {
	pthread_mutex_lock(&g_3d_lock);
	int rc = draw_locked(fb_phy, fb_w, fb_h, fb_stride, prog_handle,
	                     geom, tex, num_tex, unif, num_uniforms);
	pthread_mutex_unlock(&g_3d_lock);
	return rc;
}
