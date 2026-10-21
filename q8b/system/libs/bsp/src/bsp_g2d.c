#include <bsp/bsp_g2d.h>
#include <g2dclient/g2dclient.h>
#include <adreno/adreno_drv.h>
#include <g2d_arch.h>          /* arch_g2d_rotated_size: pure geometry helper */
#include <stdlib.h>            /* getenv */
#include <ewoksys/klog.h>

/*
 * Q8B (Radxa Dragon Q8B / SC8280XP / Adreno 690) 2D dispatch layer.
 *
 * GPU-ONLY forwarder into the Adreno 690 A2D 2D-blitter (libadreno,
 * adreno_2d.c).  Every accelerated pixel op is routed to the GPU or
 * declined; there is NO CPU/NEON rasterization inside this daemon.  A
 * declined op returns a g2d error code and the CLIENT falls back to its
 * own in-process arch_g2d engine (the driver never rasterizes on the
 * CPU - see the raspi5 GPU-only contract).
 *
 * WHAT THE A2D BLITTER DOES (source-faithful, see adreno_2d.c):
 *   - bsp_g2d_fill        -> A2D solid-color fill (SOLID_COLOR blit)
 *   - bsp_g2d_blt         -> A2D copy/scale (BLIT_OP_SCALE, rotate 0)
 *   - bsp_g2d_blt_alpha   -> A2D per-pixel source-over blend (global alpha 0xff)
 *   - bsp_g2d_scale_to    -> A2D scale (whole src -> whole dst)
 *   - bsp_g2d_rotate      -> A2D rotate, 0/180 only
 *   - bsp_g2d_gaussian_blur -> A2D bilinear scale-cascade (mip) blur
 *   - bsp_g2d_blt_phy     -> A2D copy into a raw physical scan-out dst
 *   - bsp_g2d_rotate_phy  -> A2D rotate into a raw physical dst, 0/180
 *
 * WHAT DECLINES, and why:
 *   - blt_alpha with a global alpha in (0, 0xff): the A2D blends by the
 *     per-pixel source alpha and cannot express the reference's
 *     sa = src_a * global_alpha product (no A2D constant-colour register), so
 *     it returns G2D_ERR_FAILED (-1) and the client CPU-blends that call; the
 *     0xff source-over path stays on the GPU.  A global-alpha blend needs the
 *     3D shader pipeline (M3-blocked: no GLSL->a6xx ir3 compiler in-tree).
 *   - fill_alpha with a global alpha in (0, 0xff): same A2D limitation as
 *     blt_alpha (no constant-colour blend), so it returns G2D_ERR_FAILED (-1,
 *     transient) and the client CPU-fills THAT call.  It must NOT be -2:
 *     g2dd dispatches fill and fill_alpha from the same FILL_RECT request
 *     and the client has a single G2D_CAP_FILL, so a sticky decline here
 *     would also switch the opaque GPU fill off for the rest of the session.
 *     A fully transparent request (alpha 0) is a no-op -> G2D_OK.
 *   - fill_triangle: the A2D has no triangle op, so it returns
 *     G2D_ERR_NOT_SUPPORTED (-2, sticky) and the client keeps its CPU engine
 *     (G2D_CAP_FILL_TRIANGLE is its own capability bit).
 *   - gaussian_blur runs on the GPU as a bilinear scale-cascade approximation
 *     (NOT a bit-exact per-tap gaussian - the A2D has no convolution op, and an
 *     exact gaussian needs a shader, M3-blocked); it declines -1 when the rect
 *     is degenerate or the caller's tmp scratch is too small for the mip chain.
 *   - rotate 90/270: Mesa only derives ROTATE_0/HFLIP/VFLIP/180 from
 *     src-vs-dst mirroring; there is no source-demonstrated dst-rect
 *     convention for 90/270, so they return G2D_ERR_FAILED (-1) rather
 *     than risk an out-of-bounds GPU write.  The client CPU-rotates.
 *
 * ELIGIBILITY: the A2D works on GPU-visible physical memory (CB0 is
 * identity-mapped, so pa == iova).  A buffer must be physically
 * contiguous (contig != 0, *_phy != 0), its base 64-byte aligned, and its
 * pitch a multiple of 64 (the A2D pitch fields are shr6) - for a tightly
 * packed ARGB surface pitch = w*4, so w must be a multiple of 16.  Anything
 * else returns G2D_ERR_FAILED (-1) and the client falls back.
 *
 * SAFETY GATE: nothing here is hardware-validated on SC8280XP (QEMU models
 * no Adreno) and g2dd is the shared compositor, so the GPU path is opt-in:
 * it only runs when the environment sets ADRENO_ENABLE=1 AND the Adreno
 * bring-up + CP datapath selftest succeed (adreno_2d_ready()).  Otherwise
 * every op returns G2D_ERR_NOT_SUPPORTED (-2, sticky) so clients latch the
 * capability off and stay on their in-process engine without paying an IPC
 * round trip per op.  This mirrors the bsp_g3d.c gate.
 *
 * FAILURE AFTER SUBMISSION: libadreno distinguishes a job that never reached
 * the GPU (ADRENO_E_DECLINED -> G2D_ERR_FAILED, the client may replay it on
 * the CPU) from one that was submitted and did not retire / faulted
 * (ADRENO_E_TIMEOUT / _FAULT -> G2D_ERR_TIMEOUT, which g2dclient treats like
 * FAILED but never replays: the GPU may still be writing the surface).  The
 * ring then latches the device wedged (no validated a6xx reset sequence, so
 * fail-fast like raspi5) and adreno_2d_available() flips to unavailable, so
 * every later op is -2 sticky and clients stop paying the IPC round trip.
 *
 * bsp_g2d_rotated_size is a pure geometry helper (bounding-box size for a
 * rotation), not a pixel op, so it forwards to arch_g2d_rotated_size.
 *
 * Signatures MUST stay byte-identical to the other six machines: g2dd.c is
 * shared code and a prototype divergence surfaces only as a link error.
 */

/* --- opt-in gate + capability latch (mirrors bsp_g3d.c) --- */
static int g_g2d_avail = 0;   /* 0 = unprobed, 1 = available, -1 = unavailable */

static int adreno_opted_in(void) {
	const char* e = getenv("ADRENO_ENABLE");
	return (e != NULL && e[0] == '1' && e[1] == '\0');
}

/* Probe the Adreno A2D path once (idempotent + latched).  Returns 1 when the
   GPU is opted in, brought up and passed its CP selftest; -1 otherwise.  A
   wedged device (a submitted job never retired) demotes an available latch
   to unavailable, permanently. */
static int adreno_2d_available(void) {
	if (g_g2d_avail == 0) {
		g_g2d_avail = (adreno_opted_in() && adreno_2d_ready()) ? 1 : -1;
		if (g_g2d_avail == 1)
			klog("q8b g2d: Adreno A2D blitter available\n");
	}
	if (g_g2d_avail == 1 && adreno_wedged()) {
		g_g2d_avail = -1;
		klog("q8b g2d: Adreno wedged - 2D offload disabled for this session\n");
	}
	return g_g2d_avail == 1;
}

/* Map a libadreno result onto the g2d protocol codes (g2dclient.h):
     ADRENO_OK          -> G2D_OK
     ADRENO_E_DECLINED  -> G2D_ERR_FAILED   (nothing submitted; client replays)
     TIMEOUT / FAULT /
     WEDGED             -> G2D_ERR_TIMEOUT  (submitted or device dead; the
                                            client must NOT replay - the GPU
                                            may still own the surface)
   WEDGED means an earlier job hung; this op was not submitted, but the next
   adreno_2d_available() call demotes the latch so it is reported like the
   hang rather than as a replayable miss. */
static int32_t a2d_result(int rc) {
	if (rc == ADRENO_OK)
		return G2D_OK;
	if (rc == ADRENO_E_DECLINED)
		return G2D_ERR_FAILED;
	return G2D_ERR_TIMEOUT;
}

/* A GPU blit needs a physically contiguous, GPU-visible buffer.  contig == 0
   means *_phy is 0 (see bsp_g2d.h), which the A2D cannot DMA to. */
static int gpu_buf_ok(ewokos_addr_t phy, uint8_t contig) {
	return (contig != 0 && phy != 0);
}

int32_t bsp_g2d_init(void) {
	/* 2D is optional to the daemon: probe (gated) so an opted-in board
	   reports A2D availability; never fail init. */
	(void)adreno_2d_available();
	return G2D_OK;
}

/* no engine clock reported yet (the GPU is pinned by the GMU perf table). */
uint32_t bsp_g2d_clock_hz(void) {
	return 0;
}

/* Backend-neutral GPU code the client feeds pglc_version_supported(): the
   q8b adrenocompiler backend accepts exactly 690 (Adreno 690), the same way
   raspi5's v3dcompiler accepts V3D 71.  Nothing in the shared protocol
   interprets the number any more. */
uint32_t bsp_g2d_version(void) {
	return 690;
}

int32_t bsp_g2d_cmd(int argc, char** argv, char* buf, uint32_t len) {
	(void)argc; (void)argv; (void)buf; (void)len;
	return -1;   /* no backend diagnostics of our own */
}

int32_t bsp_g2d_fill(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig, int32_t argb_w, int32_t argb_h,
			int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
	(void)argb;
	if (!adreno_2d_available())
		return G2D_ERR_NOT_SUPPORTED;
	if (!gpu_buf_ok(argb_phy, contig))
		return G2D_ERR_FAILED;
	return a2d_result(adreno_2d_fill(argb_phy, argb_w, argb_h, (uint32_t)argb_w * 4u,
			x, y, w, h, color, 0));
}

int32_t bsp_g2d_fill_alpha(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig, int32_t argb_w, int32_t argb_h,
			int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
	uint32_t a = (color >> 24) & 0xffu;
	if (a == 0)
		return G2D_OK;                    /* fully transparent: no-op */
	if (!adreno_2d_available())
		return G2D_ERR_NOT_SUPPORTED;     /* gate off / GPU down: sticky */
	if (a != 0xffu)
		return G2D_ERR_FAILED;            /* no A2D constant-colour blend: client fills THIS call (not sticky, shares G2D_CAP_FILL with the opaque path) */
	/* alpha == 0xff is an opaque fill: same as bsp_g2d_fill. */
	return bsp_g2d_fill(argb, argb_phy, contig, argb_w, argb_h, x, y, w, h, color);
}

int32_t bsp_g2d_fill_triangle(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig, int32_t argb_w, int32_t argb_h,
			int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, uint32_t color) {
	(void)argb; (void)argb_phy; (void)contig; (void)argb_w; (void)argb_h;
	(void)x1; (void)y1; (void)x2; (void)y2; (void)x3; (void)y3;
	if (((color >> 24) & 0xffu) == 0)
		return G2D_OK;                    /* fully transparent: no-op */
	/* no A2D triangle path: decline (sticky), the client rasterizes. */
	return G2D_ERR_NOT_SUPPORTED;
}

int32_t bsp_g2d_blt(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
			int32_t sx, int32_t sy, int32_t sw, int32_t sh,
			uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h,
			int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
	(void)argb_src; (void)argb_dst;
	if (!adreno_2d_available())
		return G2D_ERR_NOT_SUPPORTED;
	if (!gpu_buf_ok(src_phy, src_contig) || !gpu_buf_ok(dst_phy, dst_contig))
		return G2D_ERR_FAILED;
	return a2d_result(adreno_2d_copy(src_phy, src_w, src_h, (uint32_t)src_w * 4u,
			sx, sy, sw, sh,
			dst_phy, dst_w, dst_h, (uint32_t)dst_w * 4u,
			dx, dy, dw, dh, 0));
}

/* 1:1 copy of a clipped src rect into a RAW PHYSICAL scan-out destination
   (no dst VA in this process).  Now GPU-backed: the A2D writes the physical
   range directly.  dst_pitch is the explicit row stride (need not be w*4). */
int32_t bsp_g2d_blt_phy(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
			int32_t sx, int32_t sy, int32_t sw, int32_t sh,
			ewokos_addr_t dst_phy, uint32_t dst_size, int32_t dst_w, int32_t dst_h,
			uint32_t dst_pitch,
			int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
	(void)argb_src; (void)dst_size;
	if (!adreno_2d_available())
		return G2D_ERR_NOT_SUPPORTED;
	if (!gpu_buf_ok(src_phy, src_contig) || dst_phy == 0)
		return G2D_ERR_FAILED;
	return a2d_result(adreno_2d_copy(src_phy, src_w, src_h, (uint32_t)src_w * 4u,
			sx, sy, sw, sh,
			dst_phy, dst_w, dst_h, dst_pitch,
			dx, dy, dw, dh, 0));
}

/* rotate a src rect clockwise by degree into a RAW PHYSICAL scan-out dst.
   The A2D path supports 0/180 (rotated size == src size); 90/270 decline so
   displayd keeps its own CPU rotate + flush path. */
int32_t bsp_g2d_rotate_phy(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
		int32_t sx, int32_t sy, int32_t sw, int32_t sh,
		ewokos_addr_t dst_phy, uint32_t dst_size, int32_t dst_w, int32_t dst_h,
		uint32_t dst_pitch,
		int32_t dx, int32_t dy, int32_t degree) {
	(void)argb_src; (void)dst_size;
	if (!adreno_2d_available())
		return G2D_ERR_NOT_SUPPORTED;
	if (degree != 0 && degree != 180)
		return G2D_ERR_FAILED;            /* A2D rotate: 0/180 only */
	if (!gpu_buf_ok(src_phy, src_contig) || dst_phy == 0)
		return G2D_ERR_FAILED;
	return a2d_result(adreno_2d_copy(src_phy, src_w, src_h, (uint32_t)src_w * 4u,
			sx, sy, sw, sh,
			dst_phy, dst_w, dst_h, dst_pitch,
			dx, dy, sw, sh, degree));
}

/* Global-alpha source-over blit: dst = src*(src_a*alpha) + dst*(1-src_a*alpha)
   (see arch_g2d_blt_alpha / x86_blt_alpha_px: sa = (src_a*alpha)>>8).

   GPU path: for a global alpha of 0xff this is a plain per-pixel source-over
   (sa == src_a), which the A2D blend unit computes exactly - adreno_2d_blend
   sets RB_A2D_PIXEL_CNTL = PIXEL_OP_BLENDING with the documented SRC_ALPHA /
   ONE_MINUS_SRC_ALPHA factors (colour) and ONE / ONE_MINUS_SRC_ALPHA (alpha),
   reproducing the CPU reference.  This is the common ARGB-sprite case (e.g.
   graph_image.c blits at alpha 0xff).

   What still declines, and why:
     - 0 < alpha < 0xff: the reference multiplies the per-pixel source alpha by
       the global alpha (sa = src_a*alpha).  The A2D blend factors offer
       SRC_ALPHA and CONSTANT_ALPHA only separately - there is no composite and
       no A2D constant-colour register to modulate the sampled alpha - so the
       product is inexpressible in one A2D pass.  Return G2D_ERR_FAILED (-1,
       transient) so g2dd/the client CPU-blends this call while the 0xff GPU
       path stays latched on.  A faithful global-alpha blend needs the 3D shader
       pipeline (RB_BLEND_* constant colour), which is M3-blocked on q8b (no
       GLSL->a6xx ir3 compiler in-tree).
     - alpha == 0: fully transparent, a no-op -> G2D_OK.
     - non-contiguous / unaligned buffers: G2D_ERR_FAILED (the A2D DMAs to
       GPU-visible physical memory).
   NOTE (adreno_2d.c gap #6): a *colour* source-over through the A2D is
   register-documented but not Mesa-demonstrated (Mesa only uses A2D blending
   for D24S8 channel-preserve and rejects info->alpha_blend), so it is
   unvalidated on silicon and stays behind the ADRENO_ENABLE opt-in gate. */
int32_t bsp_g2d_blt_alpha(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
			int32_t sx, int32_t sy, int32_t sw, int32_t sh,
			uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h,
			int32_t dx, int32_t dy, int32_t dw, int32_t dh, uint8_t alpha) {
	(void)argb_src; (void)argb_dst;
	if (alpha == 0)
		return G2D_OK;                    /* fully transparent: no-op */
	if (!adreno_2d_available())
		return G2D_ERR_NOT_SUPPORTED;     /* gate off / GPU down: sticky */
	if (alpha != 0xffu)
		return G2D_ERR_FAILED;            /* src_a*alpha product not expressible: client blends */
	if (!gpu_buf_ok(src_phy, src_contig) || !gpu_buf_ok(dst_phy, dst_contig))
		return G2D_ERR_FAILED;
	return a2d_result(adreno_2d_blend(src_phy, src_w, src_h, (uint32_t)src_w * 4u,
			sx, sy, sw, sh,
			dst_phy, dst_w, dst_h, (uint32_t)dst_w * 4u,
			dx, dy, dw, dh));
}

int32_t bsp_g2d_scale_to(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
			uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h) {
	(void)argb_src; (void)argb_dst;
	if (!adreno_2d_available())
		return G2D_ERR_NOT_SUPPORTED;
	if (!gpu_buf_ok(src_phy, src_contig) || !gpu_buf_ok(dst_phy, dst_contig))
		return G2D_ERR_FAILED;
	/* whole src (0,0,src_w,src_h) -> whole dst (0,0,dst_w,dst_h).  The A2D
	   maps SRC_XMAX=src_w-1 onto DEST_BR=dst_w-1, i.e. corner-preserving,
	   which is exactly the scale_to contract. */
	return a2d_result(adreno_2d_copy(src_phy, src_w, src_h, (uint32_t)src_w * 4u,
			0, 0, src_w, src_h,
			dst_phy, dst_w, dst_h, (uint32_t)dst_w * 4u,
			0, 0, dst_w, dst_h, 0));
}

int32_t bsp_g2d_rotated_size(int32_t src_w, int32_t src_h, int32_t degree,
			int32_t* dst_w, int32_t* dst_h) {
	/* pure geometry helper (no GPU path exists for a bounding-box size). */
	return arch_g2d_rotated_size(src_w, src_h, degree, dst_w, dst_h);
}

int32_t bsp_g2d_rotate(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
			uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h, int32_t degree) {
	(void)argb_src; (void)argb_dst;
	if (!adreno_2d_available())
		return G2D_ERR_NOT_SUPPORTED;
	if (degree != 0 && degree != 180)
		return G2D_ERR_FAILED;            /* A2D rotate: 0/180 only */
	if (!gpu_buf_ok(src_phy, src_contig) || !gpu_buf_ok(dst_phy, dst_contig))
		return G2D_ERR_FAILED;
	/* rotate the whole source; for 0/180 the dst content size == src size. */
	return a2d_result(adreno_2d_copy(src_phy, src_w, src_h, (uint32_t)src_w * 4u,
			0, 0, src_w, src_h,
			dst_phy, dst_w, dst_h, (uint32_t)dst_w * 4u,
			0, 0, src_w, src_h, degree));
}

/* Whole-surface separable gaussian blur, GPU-accelerated.  The a6xx A2D
   blitter has NO convolution op (CP_BLIT is only FILL/COPY/SCALE) and no
   kernel-weight register, so an exact per-tap gaussian needs a compute/fragment
   shader (M3-blocked on q8b: no GLSL->a6xx ir3 compiler in-tree).  What the
   A2D DOES have is a bilinear FILTER on BLIT_OP_SCALE ("used for mipmap
   generation"), so adreno_2d_blur builds a real GPU blur the way raspi's own
   GPU blur does it (downsample -> upsample): halve the rect L times (~2^L =
   radius) into a scratch mip chain, then bilinear-upsample it back in place.
   Repeated 2-tap averaging converges to a gaussian (CLT); the result is a
   genuine GPU blur, an APPROXIMATION of arch_g2d_gaussian's kernel (not a
   bit-exact match - see adreno_2d.c gap #7).  The canvas is written only by the
   final upsample, so a mid-chain failure leaves it pristine for the client's
   CPU fallback.

   Scratch: g2dd guarantees tmp.size >= (rect_h-1)*argb_w*4 + rect_w*4 and hands
   us tmp_phy/tmp_contig; we pass that exact bound to adreno_2d_blur, which lays
   the mip chain out inside it and declines if it does not fit (no assumption
   about tmp's real allocation).  Anything ineligible (gate off, non-contig /
   unaligned buffers, degenerate rect) declines so the client CPU-blurs. */
int32_t bsp_g2d_gaussian_blur(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig,
			uint32_t* tmp, ewokos_addr_t tmp_phy, uint8_t tmp_contig,
			int32_t argb_w, int32_t argb_h,
			int32_t rect_x, int32_t rect_y,
			int32_t rect_w, int32_t rect_h,
			int32_t radius) {
	uint64_t tmp_limit;
	(void)argb; (void)tmp;
	if (!adreno_2d_available())
		return G2D_ERR_NOT_SUPPORTED;     /* gate off / GPU down: sticky */
	if (!gpu_buf_ok(argb_phy, contig) || !gpu_buf_ok(tmp_phy, tmp_contig))
		return G2D_ERR_FAILED;
	if (radius < 1 || rect_w < 2 || rect_h < 2)
		return G2D_ERR_FAILED;            /* degenerate: the client CPU-blurs */
	/* g2dd guarantees tmp.size >= (rect_h-1)*argb_w*4 + rect_w*4; use that as
	   the scratch budget so adreno_2d_blur can prove the mip chain fits. */
	tmp_limit = (uint64_t)(rect_h - 1) * (uint64_t)argb_w * 4u
			+ (uint64_t)rect_w * 4u;
	return a2d_result(adreno_2d_blur(argb_phy, argb_w, argb_h, (uint32_t)argb_w * 4u,
			rect_x, rect_y, rect_w, rect_h, radius,
			tmp_phy, tmp_limit));
}

/* portablegl 3D offload lives in bsp_g3d.c (<bsp/bsp_g3d.h>): clear routes to
   the same Adreno A2D solid fill; draw/program still decline (no GLSL->a6xx
   ir3 compiler in-tree, the M3 blocker). */
