#include <bsp/bsp_g2d.h>
#include <g2dclient/g2dclient.h>
#include <adreno/adreno_drv.h>
#include <g2d_arch.h>          /* arch_g2d_rotated_size: pure geometry helper */
#include <ewoksys/klog.h>
#include <math.h>

/*
 * Q8B (Radxa Dragon Q8B / SC8280XP / Adreno 690) 2D dispatch layer.
 *
 * GPU-ONLY forwarder into the Adreno 690 A2D/compute paths (libadreno).
 * Every accelerated pixel op is routed to the GPU or
 * declined; there is NO CPU/NEON rasterization inside this daemon.  A
 * declined op returns a g2d error code and the CLIENT falls back to its
 * own in-process arch_g2d engine (the driver never rasterizes on the
 * CPU - see the raspi5 GPU-only contract).
 *
 * DISPATCH (see adreno_2d.c and adreno_3d.c):
 *   - bsp_g2d_fill        -> A2D opaque fill; compute fractional-alpha fill
 *   - bsp_g2d_blt         -> A2D copy / exact nearest compute scale
 *   - bsp_g2d_blt_alpha   -> exact straight-alpha compute blend
 *   - bsp_g2d_scale_to    -> exact Q15 nearest compute scale
 *   - bsp_g2d_rotate      -> A2D 0/180; compute other angles/packed pitches
 *   - bsp_g2d_gaussian_blur -> explicit decline (exact convolution pending)
 *   - bsp_g2d_blt_phy     -> A2D copy into a raw physical scan-out dst
 *   - bsp_g2d_rotate_phy  -> A2D rotate into a raw physical dst, 0/180
 *
 * WHAT DECLINES, and why:
 *   - fill_triangle: the A2D has no triangle op, so it returns
 *     G2D_ERR_NOT_SUPPORTED (-2, sticky) and the client keeps its CPU engine
 *     (G2D_CAP_FILL_TRIANGLE is its own capability bit).
 *   - gaussian_blur declines -2: the private A2D mip approximation does not
 *     implement the protocol's Q16 Gaussian kernel. Clients use their own
 *     exact software path until a conformant compute kernel is available.
 *   - raw physical rotate 90/270: the A2D destination convention is not
 *     validated; return FAILED without writes and let the client rotate.
 *     Canvas rotation uses an exact integer compute mapping instead.
 *
 * ELIGIBILITY: the A2D works on GPU-visible physical memory (CB0 is
 * identity-mapped, so pa == iova).  A buffer must be physically
 * contiguous (contig != 0, *_phy != 0), its base 64-byte aligned, and its
 * pitch a multiple of 64 (the A2D pitch fields are shr6) - for a tightly
 * packed ARGB surface pitch = w*4, so w must be a multiple of 16.  Anything
 * else returns G2D_ERR_FAILED (-1) and the client falls back. Compute copy
 * and blend/rotation use aligned bases with arbitrary packed row widths and
 * decline overlapping source/destination allocations before submission.
 * Fractional fill reads and writes only the invocation's destination pixel.
 *
 * SAFETY GATE: fill/copy/180 rotation and uniform scaling have passed Q8B
 * pixel/guard readback. Further API coverage is tracked in test/HANDOVER.md.
 * The GPU path remains opt-in:
 * it only runs when the environment sets ADRENO_ENABLE=1 AND the Adreno
 * bring-up + CP datapath selftest succeed (adreno_2d_ready()).  Otherwise
 * every op returns G2D_ERR_NOT_SUPPORTED (-2, sticky) so clients latch the
 * capability off and stay on their in-process engine without paying an IPC
 * round trip per op.  This mirrors the bsp_g3d.c gate.
 *
 * FAILURE AFTER SUBMISSION: libadreno distinguishes a job that never reached
 * the GPU (ADRENO_E_DECLINED -> G2D_ERR_FAILED, the client may replay it on
 * the CPU) from one that was submitted and did not retire / faulted
 * (ADRENO_E_TIMEOUT / _FAULT -> G2D_ERR_TIMEOUT, which callers must never
 * replay: the GPU may still be writing the surface).  The
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
	/* Match the 3D gate: serial tests use /tmp/adreno.cfg because the
	   board's command transport does not provide a POSIX shell. */
	return adreno_flag("ADRENO_ENABLE");
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
	if (!gpu_buf_ok(argb_phy, contig))
		return G2D_ERR_FAILED;
	if (a != 0xffu)
		return a2d_result(adreno_compute_fill_alpha(argb_phy,argb_w,argb_h,x,y,w,h,color));
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
	if (sw != dw || sh != dh)
		return a2d_result(adreno_compute_scale(src_phy, src_w, src_h,
			sx, sy, sw, sh, dst_phy, dst_w, dst_h, dx, dy, dw, dh, 0));
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

/* Match arch_g2d_blt_alpha, including (src_a*global)>>8, opaque copy,
   transparent destination RGB and integer /255 rounding. The A2D blend
   failed transparent-destination readback; use the regular compute kernel. */
int32_t bsp_g2d_blt_alpha(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
			int32_t sx, int32_t sy, int32_t sw, int32_t sh,
			uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h,
			int32_t dx, int32_t dy, int32_t dw, int32_t dh, uint8_t alpha) {
	(void)argb_src; (void)argb_dst;
	if (alpha == 0)
		return G2D_OK;                    /* fully transparent: no-op */
	if (!adreno_2d_available())
		return G2D_ERR_NOT_SUPPORTED;     /* gate off / GPU down: sticky */
	if (!gpu_buf_ok(src_phy, src_contig) || !gpu_buf_ok(dst_phy, dst_contig))
		return G2D_ERR_FAILED;
	/* A2D fails straight-alpha semantics on an empty destination. Use the
	   integer kernel for every global alpha and destination alpha instead. */
	return a2d_result(adreno_compute_blend(src_phy, src_w, src_h,
			sx, sy, sw, sh, dst_phy, dst_w, dst_h, dx, dy, dw, dh, alpha));
}

int32_t bsp_g2d_scale_to(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
			uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h) {
	(void)argb_src; (void)argb_dst;
	if (!adreno_2d_available())
		return G2D_ERR_NOT_SUPPORTED;
	if (!gpu_buf_ok(src_phy, src_contig) || !gpu_buf_ok(dst_phy, dst_contig))
		return G2D_ERR_FAILED;
	/* A2D samples pixel centres and fails the corner-preserving Q15 oracle.
	   The integer CS kernel implements that mapping without interpolation. */
	return a2d_result(adreno_compute_scale(src_phy, src_w, src_h,
			0, 0, src_w, src_h, dst_phy, dst_w, dst_h,
			0, 0, dst_w, dst_h, 1));
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
	if (!gpu_buf_ok(src_phy, src_contig) || !gpu_buf_ok(dst_phy, dst_contig))
		return G2D_ERR_FAILED;
	degree=((degree%360)+360)%360;
	if ((degree != 0 && degree != 180) || (src_w & 15) || (dst_w & 15)) {
		/* Geometry only on CPU; every output pixel is written by compute.
		   round(sin/cos * 16384) matches arch_g2d's integer-degree table. */
		double radians=(double)degree*(3.14159265358979323846/180.0);
		int32_t c=(int32_t)round(cos(radians)*16384.0);
		int32_t s=(int32_t)round(sin(radians)*16384.0);
		return a2d_result(adreno_compute_rotate(src_phy,src_w,src_h,
			dst_phy,dst_w,dst_h,degree,c,s));
	}
	/* rotate the whole source; for 0/180 the dst content size == src size. */
	return a2d_result(adreno_2d_copy(src_phy, src_w, src_h, (uint32_t)src_w * 4u,
			0, 0, src_w, src_h,
			dst_phy, dst_w, dst_h, (uint32_t)dst_w * 4u,
			0, 0, src_w, src_h, degree));
}

/* Exact Q16 Gaussian needs a compute kernel. Decline without touching either
   surface; the independent client capability bit preserves other GPU ops. */
int32_t bsp_g2d_gaussian_blur(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig,
			uint32_t* tmp, ewokos_addr_t tmp_phy, uint8_t tmp_contig,
			int32_t argb_w, int32_t argb_h,
			int32_t rect_x, int32_t rect_y,
			int32_t rect_w, int32_t rect_h,
			int32_t radius) {
	(void)argb; (void)argb_phy; (void)contig;
	(void)tmp; (void)tmp_phy; (void)tmp_contig;
	(void)argb_w; (void)argb_h; (void)rect_x; (void)rect_y;
	(void)rect_w; (void)rect_h; (void)radius;
	/* A mip cascade is not the Q16 Gaussian promised by this API. */
	return G2D_ERR_NOT_SUPPORTED;
}

/* PortableGL clear/program/compute dispatch live in bsp_g3d.c. */
