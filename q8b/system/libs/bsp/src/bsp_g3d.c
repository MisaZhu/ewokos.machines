/*
 * bsp_g3d.c - Q8B (Adreno 690) portablegl 3D offload backend forwarder.
 *
 * Q8B is the second EwokOS machine with real 3D GPU hardware (after
 * raspi5's V3D 7.1), so per the bsp_g3d contract this file is a backend
 * forwarder into the Adreno 690 driver (machines/q8b/system/libs/adreno,
 * libadreno.a) rather than a dead decline-only stub.
 *
 * WHAT IS IMPLEMENTED (source-faithful + compile-verified):
 *   The full GPU bring-up datapath lives in libadreno: reg map -> gpucc
 *   clocks -> SMMU CB0 identity + CB1 GMU map -> GMU boot + HFI -> CP
 *   ringbuffer -> SQE load -> a6xx hw_init -> secure-mode exit -> a
 *   CP_MEM_WRITE round-trip selftest that proves an indirect buffer can
 *   be submitted on the CP ring and its fence retired.  bsp_g3d_init()
 *   drives that bring-up via adreno_3d_init() and latches the result.
 *   bsp_g3d_clear routes a full-frame ARGB8888 clear through the a6xx
 *   A2D solid-color blitter (adreno_2d_fill, libadreno adreno_2d.c): a
 *   CP_SET_MARKER/CP_WAIT_FOR_IDLE prologue, GRAS/RB/SP/TPL1 A2D register
 *   writes via PKT4 and a CP_BLIT(BLIT_OP_SCALE) trigger, all emitted
 *   from the mesa freedreno fd6_blitter.cc sequence and verified against
 *   a6xx.xml.  It is gated the same way (ADRENO_ENABLE + bring-up) and is
 *   NOT hardware-validated on SC8280XP (QEMU models no Adreno); the
 *   fill-channel byte order, the skipped RB_DBG_ECO_CNTL quirk RMW, the
 *   RB_CCU_CNTL=0 assumption and the A2D-usable-right-after-CP assumption
 *   are documented as confirmation gaps in adreno_2d.c.
 *
 *   bsp_g3d_program / program_free / draw forward into the a6xx COMPUTE
 *   back end (adreno_3d.c, libadreno): a program is a precompiled a6xx
 *   ir3 blob in the adreno_prog.h VERSION 2 format (ADRENO_PROG_MAGIC),
 *   produced app-side by machines/q8b/system/libs/adrenocompiler (the
 *   shared pglcompiler.h contract over the vendored mesa freedreno/ir3
 *   tree, GLSL->NIR->ir3->a6xx; this is what q8b's `export PGL_G3D=yes`
 *   compiles portablegl's shaders with), uploaded to GPU buffers, and a
 *   draw dispatches it as compute - the sanctioned CSD/CS route for
 *   Adreno, not the binner / fixed-function 3D pipeline:
 *     - a plain program (is_geom == 0) is one fullscreen kernel that
 *       writes the framebuffer one invocation per pixel;
 *     - a geometry program (is_geom 1..4: gouraud / textured / instanced /
 *       colour-modulated textured) is the same two-pass compute rasterizer
 *       raspi5 runs on V3D (pglcompiler's geom kernels): pass 1 (kern[1])
 *       transforms every vertex into a GPU vout buffer, pass 2 (kern[0])
 *       rasterizes one triangle (or geom_tpw triangles) per workgroup into
 *       the framebuffer + depth buffer, sampling texture unit 0 when the
 *       kind is textured.  mode / nverts / pos / col / z / ctex /
 *       first_frame / clear_rgba are unpacked here into adreno_3d_geom_t.
 *   The dispatch is emitted from turnip's A6XX CS sequence
 *   (tu6_emit_cs_config / tu6_emit_xs / tu6_emit_xs_constants +
 *   tu_cmd_buffer.cc's NDRANGE/CP_EXEC_CS path) with the A690 device
 *   props (num_sp_cores=8, fibers_per_sp=4096, instr_cache_size=127,
 *   has_lpac, supports_double_threadsize), SSBOs bound through the CS UAV
 *   aperture (SP_CS_UAV_BASE) exactly as gallium fd6 does.  It is gated
 *   the same way (ADRENO_ENABLE + bring-up) and is NOT hardware-validated
 *   on SC8280XP; the remaining confirmation gaps are numbered in
 *   adreno_3d.c.
 *
 * WHAT STILL DECLINES (-2), and why - stated honestly:
 *   bsp_g3d_program declines a blob whose magic is not ADRENO_PROG_MAGIC
 *   (a V3D-QPU G2D_PGL_PROGRAM_MAGIC blob from a client built without the
 *   Adreno compiler is meaningless here) or that fails adreno_3d.c's v2
 *   header validation.  bsp_g3d_draw declines when there is no compiled
 *   program (prog_handle < 0: the CS kernel IS the whole effect, there is
 *   no fixed-function fs_id path) or a non-ARGB8888 target (the CS back end
 *   binds the framebuffer as a linear R32_UINT buffer view); a geometry
 *   draw that is not GL_TRIANGLES, exceeds the vout capacity, or has a
 *   non-contiguous / unaligned input buffer is declined (-1, replayable)
 *   inside adreno_3d.c before anything is submitted.  bsp_g3d_clear
 *   declines a non-ARGB8888 target (fmt != G2D_PGL_FMT_RGBA8, e.g. BGR565):
 *   the A2D fill path here is wired for FMT6_8_8_8_8_UNORM only.
 *   Declining -2 makes portablegl keep its own proven in-process
 *   CPU/NEON rasterizer, which is strictly faster than an IPC round trip
 *   to a software clear/draw inside this daemon.
 *
 * SAFETY GATE: nothing here is hardware-validated on SC8280XP (QEMU
 * models no Adreno).  The bring-up writes real GPU/GMU/SMMU MMIO and
 * g2dd is the shared compositor, so an unvalidated sequence must not be
 * able to wedge the display.  The Adreno probe therefore only runs when
 * the environment explicitly opts in with ADRENO_ENABLE=1; otherwise
 * bsp_g3d_init() is a benign no-op and every op declines -2.  Set
 * ADRENO_ENABLE=1 on the board to exercise bring-up + the datapath
 * selftest and read the outcome from klog.
 *
 * FAILURE AFTER SUBMISSION: a clear/draw that libadreno rejected before
 * touching the GPU (ADRENO_E_DECLINED) maps to G2D_ERR_FAILED (-1) and
 * portablegl replays it on the CPU.  One that WAS submitted and did not
 * retire or faulted (ADRENO_E_TIMEOUT / _FAULT) maps to G2D_ERR_TIMEOUT
 * (-3): the client treats it like FAILED but must never replay it, since
 * the GPU may still own the framebuffer.  The ring latches the device
 * wedged (fail-fast, no validated a6xx reset) and g3d_probe() then reports
 * -1 so every later op declines -2 sticky.
 *
 * Signatures MUST stay byte-identical to the other six machines: g2dd.c
 * is shared code and a prototype divergence surfaces only as a link
 * error.
 */

#include <bsp/bsp_g3d.h>
#include <g2dclient/g2dclient.h>   /* G2D_PGL_FMT_RGBA8, G2D_ERR_* */
#include <adreno/adreno_drv.h>
#include <adreno/adreno_prog.h>    /* ADRENO_PROG_MAGIC, adreno_prog_blob_t */
#include <stdlib.h>
#include <string.h>
#include <ewoksys/klog.h>

/* capability latch: 0 = not probed, 1 = datapath ready, -1 = declined */
static int g_g3d_ready = 0;

static int adreno_opted_in(void) {
	/* getenv("ADRENO_ENABLE") == "1" (init.rd) or an ADRENO_ENABLE=1 line in
	   /tmp/adreno.cfg, so an ssh-based harness can trigger the bring-up
	   without a shell. */
	return adreno_flag("ADRENO_ENABLE");
}

/* Probe the Adreno 690 datapath once (idempotent + latched).  Returns 1
   when the GPU brought up and passed its CP_MEM_WRITE selftest, -1 when
   declined (not opted in, or bring-up/selftest failed).  This validates
   the submission datapath only - it does NOT enable a pixel path, which
   clear/draw still decline for the reasons in the header comment. */
static int g3d_probe(void) {
	if(g_g3d_ready == 1 && adreno_wedged()) {
		/* a submitted job never retired: demote for the rest of the session */
		g_g3d_ready = -1;
		klog("q8b g3d: Adreno wedged - 3D offload disabled for this session\n");
	}
	if(g_g3d_ready != 0)
		return g_g3d_ready;
	if(!adreno_opted_in()) {
		g_g3d_ready = -1;
		return -1;
	}
	int rc = adreno_3d_init();
	g_g3d_ready = (rc == 0) ? 1 : -1;
	klog("q8b g3d: adreno bring-up %s (rc=%d)\n",
			(g_g3d_ready == 1) ? "OK" : "DECLINED", rc);
	return g_g3d_ready;
}

/* libadreno result -> g2d code.  DECLINED (nothing submitted) is a
   replayable -1; TIMEOUT / FAULT / WEDGED are -3 so the client does not
   replay onto a surface the GPU may still be writing (same mapping as
   bsp_g2d.c a2d_result). */
static int32_t a3d_result(int rc) {
	if (rc == ADRENO_OK)
		return G2D_OK;
	if (rc == ADRENO_E_DECLINED)
		return G2D_ERR_FAILED;
	return G2D_ERR_TIMEOUT;
}

int32_t bsp_g3d_init(void) {
	/* 3D is optional: g2dd ignores this return value.  Probe (gated) so
	   an opted-in board reports bring-up status; never fail init. */
	(void)g3d_probe();
	return 0;
}

int32_t bsp_g3d_clear(uint32_t* fb, ewokos_addr_t fb_phy, uint8_t contig,
		int32_t w, int32_t h, uint32_t stride, uint32_t fmt,
		uint32_t color, int32_t rb_swap) {
	(void)fb;
	if (adreno_wedged())
		return G2D_ERR_TIMEOUT;
	/* The A2D fill path is wired for ARGB8888 (FMT6_8_8_8_8_UNORM) only;
	   a BGR565 (or other) target declines so portablegl keeps its CPU
	   clear.  fmt uses the G2D_PGL_* codes (g2dclient.h). */
	if (fmt != G2D_PGL_FMT_RGBA8)
		return -2 /* G2D_ERR_NOT_SUPPORTED */;
	/* Gate: only an opted-in board whose Adreno bring-up + CP selftest
	   succeeded may submit.  Otherwise decline (sticky) so the client
	   latches the 3D path off and stays on its in-process rasterizer. */
	if (g3d_probe() != 1)
		return -2 /* G2D_ERR_NOT_SUPPORTED */;
	/* The A2D DMAs to GPU-visible physical memory (CB0 identity map, so
	   pa == iova): the target must be physically contiguous with a
	   resolved base.  stride is the byte row pitch (passed straight to
	   the A2D dst pitch, which requires %64). */
	if (!contig || fb_phy == 0)
		return G2D_ERR_FAILED;
	return a3d_result(adreno_2d_fill(fb_phy, w, h, stride, 0, 0, w, h, color,
			rb_swap));
}

int32_t bsp_g3d_draw(ewokos_addr_t fb_phy, uint8_t fb_contig,
		int32_t w, int32_t h, uint32_t stride, uint32_t fmt,
		uint32_t mode, uint32_t nverts,
		ewokos_addr_t pos_phy, uint8_t pos_contig,
		ewokos_addr_t col_phy, uint8_t col_contig,
		uint32_t fs_id, uint32_t color, int32_t rb_swap,
		uint32_t depth_func, int32_t depth_mask,
		int32_t blend_en, int32_t cull_en,
		ewokos_addr_t z_phy, uint8_t z_contig,
		uint32_t z_stride, uint8_t first_frame,
		uint32_t clear_rgba, float clear_z, const void* ext, const void* tex,
		int32_t prog_handle, const void* unif, uint32_t num_uniforms,
		const void* ctex) {
	adreno_3d_geom_t g;
	adreno_3d_tex_t atex[G2D_PGL_MAX_TEX_UNITS];
	uint32_t num_tex = 0;
	/* The compiled kernel carries its own depth / blend / cull / texture
	   state (the fixed-function fs_id path does not exist on the CS back
	   end), so those legacy fields are not consumed here; void-cast to
	   keep the shared signature byte-identical across the seven machines. */
	(void)fs_id; (void)color; (void)rb_swap; (void)depth_func;
	(void)depth_mask; (void)blend_en; (void)cull_en; (void)clear_z;
	(void)ext; (void)tex;

	if (adreno_wedged())
		return G2D_ERR_TIMEOUT;
	if (g3d_probe() != 1)
		return G2D_ERR_NOT_SUPPORTED;
	/* The CS back end binds the framebuffer as a linear R32_UINT buffer
	   view, so only a uint32 ARGB8888 target is dispatchable; a BGR565 (or
	   other) target declines so portablegl keeps its CPU rasterizer. */
	if (fmt != G2D_PGL_FMT_RGBA8)
		return G2D_ERR_NOT_SUPPORTED;
	/* A compiled Adreno program is mandatory: the CS kernel IS the effect,
	   there is no fixed-function fs_id path.  A V3D-QPU-only client (no
	   prog_handle) declines. */
	if (prog_handle < 0)
		return G2D_ERR_NOT_SUPPORTED;
	/* The CS DMAs to GPU-visible physical memory (CB0 identity map, pa ==
	   iova): the target must be physically contiguous with a resolved base. */
	if (!fb_contig || fb_phy == 0)
		return G2D_ERR_FAILED;

	/* Geometry inputs for an is_geom program (adreno_3d.c ignores them for
	   a plain fullscreen program).  Every buffer the CS reads must be
	   physically contiguous (CB0 identity map): a non-contiguous one is
	   passed as 0 so the daemon declines (-1, replayable) rather than
	   DMAing a scattered allocation. */
	g.mode        = mode;
	g.nverts      = nverts;
	g.pos_phy     = (pos_contig && pos_phy) ? (uint64_t)pos_phy : 0;
	g.col_phy     = (col_contig && col_phy) ? (uint64_t)col_phy : 0;
	g.z_phy       = (z_contig && z_phy) ? (uint64_t)z_phy : 0;
	g.z_stride    = z_stride;
	g.ctex_phy    = 0;
	g.ctex_stride = 0;
	g.ctex_w      = 0;
	g.ctex_h      = 0;
	/* ctex is the caller's g2d_pgl_ctex_t (g2dd fills it when the client
	   bound sampler-unit canvases; NULL from an older caller means no unit
	   bound).  Two consumers: the geometry kernels read unit 0 as an SSBO
	   (g.ctex_*, raspi5 parity); a plain fullscreen kernel samples every
	   bound unit through real a6xx texture+sampler descriptors, so all
	   G2D_PGL_MAX_TEX_UNITS entries are forwarded (unit i -> texture_index
	   i).  A unit that is unbound or non-contiguous is passed with phy == 0
	   so the daemon samples a zeroed dummy rather than DMAing a scattered
	   allocation. */
	memset(atex, 0, sizeof(atex));
	if (ctex) {
		const g2d_pgl_ctex_t* c = (const g2d_pgl_ctex_t*)ctex;
		uint32_t u;
		for (u = 0; u < G2D_PGL_MAX_TEX_UNITS; u++) {
			atex[u].phy        = (c->ctex_contig[u] && c->ctex_phy[u])
			                        ? (uint64_t)c->ctex_phy[u] : 0;
			atex[u].stride     = c->ctex_stride[u];
			atex[u].w          = c->ctex_w[u];
			atex[u].h          = c->ctex_h[u];
			atex[u].tiling     = c->ctex_tiling[u];
			atex[u].ub_pad     = c->ctex_ub_pad[u];
			atex[u].depth      = c->ctex_depth[u];
			atex[u].arrstride64 = c->ctex_arrstride64[u];
		}
		num_tex = G2D_PGL_MAX_TEX_UNITS;
		/* These compute kernels read linear words, not V3D tiled images or
		   layered textures. Leave incompatible unit-0 bindings unresolved. */
		if (c->ctex_contig[0] && c->ctex_phy[0] &&
		    c->ctex_tiling[0] == 0 && c->ctex_depth[0] <= 1 &&
		    c->ctex_w[0] <= 0xffffffffu / 4u) {
			g.ctex_phy    = (uint64_t)c->ctex_phy[0];
			g.ctex_stride = c->ctex_stride[0] ? c->ctex_stride[0] : c->ctex_w[0] * 4u;
			g.ctex_w      = c->ctex_w[0];
			g.ctex_h      = c->ctex_h[0];
		}
	}
	g.first_frame = first_frame;
	g.clear_rgba  = clear_rgba;

	return a3d_result(adreno_3d_draw(fb_phy, w, h, stride, prog_handle, &g,
			atex, num_tex, (const uint32_t*)unif, num_uniforms));
}

int32_t bsp_g3d_program(const void* blob, uint32_t size) {
	const adreno_prog_blob_t* h;
	int handle;
	if (adreno_wedged())
		return G2D_ERR_TIMEOUT;
	if (g3d_probe() != 1)
		return G2D_ERR_NOT_SUPPORTED;
	if (blob == NULL || size < sizeof(adreno_prog_blob_t))
		return G2D_ERR_NOT_SUPPORTED;
	h = (const adreno_prog_blob_t*)blob;
	/* Only an Adreno a6xx ir3 blob is dispatchable.  portablegl's wire blob
	   carries V3D-QPU words (G2D_PGL_PROGRAM_MAGIC); declining it (sticky)
	   keeps the client on its CPU rasterizer rather than feeding an Adreno
	   a VideoCore ISA it cannot run. */
	if (h->magic != ADRENO_PROG_MAGIC)
		return G2D_ERR_NOT_SUPPORTED;
	handle = adreno_3d_program_load(blob, size);
	if (handle < 0)
		return G2D_ERR_FAILED;
	return (int32_t)handle;
}

int32_t bsp_g3d_program_free(int32_t handle) {
	if (adreno_wedged())
		return G2D_ERR_TIMEOUT;
	if (g3d_probe() != 1)
		return G2D_ERR_NOT_SUPPORTED;
	return adreno_3d_program_free(handle) == 0
			? G2D_OK : G2D_ERR_NOT_SUPPORTED;
}
