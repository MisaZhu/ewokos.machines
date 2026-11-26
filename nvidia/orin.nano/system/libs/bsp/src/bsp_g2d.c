#include <bsp/bsp_g2d.h>
#include <bsp/bsp_vic.h>
#include <g2dclient/g2dclient.h>
#include <g2d_arch.h>
#include <ewoksys/klog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Orin Nano (Tegra234 VIC) 2D dispatch layer — hybrid model.
 *
 * The shared aarch64 NEON engine (arch_g2d_*) implements every op, so
 * g2dd always answers with real pixels.  The VIC hardware path takes
 * over the eligible subset when it is opted in AND has passed bring-up
 * plus a DMA fill/blit readback selftest (bsp_vic_2d_enable); rotation
 * additionally needs its own selftest latch (bsp_vic_2d_can_rotate):
 *   - bsp_g2d_blt (1:1)   -> VIC single-slot copy
 *   - bsp_g2d_rotate      -> VIC flip/transpose (0/90/180/270 only)
 *   - bsp_g2d_blt_phy     -> VIC copy into the raw physical scan-out FB
 *   - bsp_g2d_rotate_phy  -> VIC rotate into the raw physical scan-out FB
 * Everything else (fill, alpha blend, scale, blur, triangle, arbitrary-
 * angle rotate, non-contig buffers) stays on the NEON engine: the VIC
 * blend and polyphase scaler do not reproduce the arch engine's exact
 * integer pixel math, and a VIC fill is slower than the CPU at every size
 * (the engine is memory-bandwidth bound like the CPU, plus job setup).
 *
 * GATE: the hardware path stays closed until /tmp/vic2d.cfg or the
 * environment opts in with VIC2D_ENABLE=1.  Default off = pure software,
 * byte-identical to the CPU-only desktop.  Mirrors the QCOM8GEN3 ADRENO_ENABLE
 * gate.
 *
 * FAILURE MODEL: a VIC op declined before submission or rejected by the
 * firmware (-1) falls back to the NEON engine in-process (the engine is
 * idle and the CPU redraws the whole rect).  A job submitted
 * but not retired (-3) returns G2D_ERR_TIMEOUT and latches the VIC path
 * off for the session: the engine may still own the surface, so it must
 * never be CPU-replayed (g2dclient contract).  Raw-physical ops have no
 * dst VA in this daemon, so a decline there answers G2D_ERR_FAILED and
 * displayd keeps its own memcpy flush; with the VIC path unavailable
 * they answer the sticky G2D_ERR_NOT_SUPPORTED so displayd stops asking.
 *
 * Signatures MUST stay byte-identical to the other machines: g2dd.c is
 * shared code and a prototype divergence surfaces only as a link error.
 */

/* --- opt-in gate + capability latch --- */
static int g_vic_avail = 0;   /* 0 = unprobed, 1 = available, -1 = unavailable */

/* jobs the VIC engine completed, by op (devcmd /dev/g2d stat) */
static uint32_t g_n_blt, g_n_rot, g_n_phy;

static int vic2d_opted_in(void) {
	const char* v = getenv("VIC2D_ENABLE");
	if (v != NULL && v[0] == '1' && v[1] == '\0')
		return 1;
	FILE* f = fopen("/tmp/vic2d.cfg", "rb");
	if (f == NULL)
		return 0;
	char buf[128];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return strstr(buf, "VIC2D_ENABLE=1") != NULL;
}

static int vic_available(void) {
	if (g_vic_avail == 0) {
		if (!vic2d_opted_in()) {
			g_vic_avail = -1;
		} else {
			g_vic_avail = (bsp_vic_2d_enable() == 0) ? 1 : -1;
			if (g_vic_avail == 1)
				klog("orin g2d: VIC 2D path available\n");
			else
				klog("orin g2d: VIC bring-up failed - software 2D only\n");
		}
	}
	/* bsp_vic can latch itself off later (stalled or rejected jobs) */
	return g_vic_avail == 1 && bsp_vic_2d_available();
}

/* A submitted job that never retired: the engine may still be writing
 * the surface.  Latch the VIC path off and report the terminal code. */
static int32_t vic_wedge(void) {
	g_vic_avail = -1;
	klog("orin g2d: VIC job timeout - 2D offload disabled for this session\n");
	return G2D_ERR_TIMEOUT;
}

/* VIC DMA needs physically contiguous, visible buffers with a 256B
 * aligned base and pitch (TRM §7.3.2.1.6: address bits [39:8] only; the
 * engine fetches on a 256B pitch grid), so a canvas whose width is not a
 * multiple of 64 pixels stays on the NEON engine.  Which heights and
 * rect parities the engine reproduces exactly is settled by the bsp_vic
 * selftest; bsp_vic declines (-1) what it has not proven. */
#define VIC_ALIGN 256u
static int vic_buf_ok(ewokos_addr_t phy, uint8_t contig, int32_t w) {
	return contig != 0 && phy != 0 && (phy & (VIC_ALIGN - 1)) == 0 &&
			((uint32_t)w * 4u & (VIC_ALIGN - 1)) == 0;
}

static int vic_rect_ok(int32_t x, int32_t y, int32_t w, int32_t h) {
	return x >= 0 && y >= 0 && w > 0 && h > 0;
}

/* A VIC copy costs a fixed ~260us before the first pixel, then ~0.6ns
 * per pixel; the NEON engine copies the uncached contiguous canvases at
 * ~3.7ns/px (g2dtest break-even, measured on the board: 256x256 309us vs
 * 245us, 384x384 363us vs 544us).  Smaller ops stay on the NEON engine.
 * The scan-out pushes have no floor: their fallback is displayd's
 * whole-frame memcpy out of the uncached canvas (63ms at 3840x2160
 * against 5ms through the engine). */
#define VIC_MIN_COPY_PX (320 * 320)

static int vic_worth(int32_t w, int32_t h, int32_t min_px) {
	return w > 0 && h > 0 && (int64_t)w * h >= min_px;
}

int32_t bsp_g2d_init(void) {
	/* Software engine always works; probe the gated VIC path so an
	   opted-in board reports availability once.  Never fail init. */
	arch_g2d_init();
	(void)vic_available();
	return G2D_OK;
}

uint32_t bsp_g2d_clock_hz(void) {
	if (vic_available())
		return bsp_vic_2d_clock_hz();
	return 0;
}

/* compiler target for the portablegl GPU path: the GA10B geometry
   rasterizer once bsp_g3d brought the GPU up, else none (2D only) */
uint32_t bsp_g3d_version(void);

uint32_t bsp_g2d_version(void) {
	return bsp_g3d_version();
}

int32_t bsp_g2d_cmd(int argc, char** argv, char* buf, uint32_t len) {
	if (argc > 0 && argv && argv[0] && strcmp(argv[0], "stat") == 0) {
		snprintf(buf, len, "VIC 2D: %s (gate %s) jobs blt=%u rotate=%u scanout=%u",
				g_vic_avail == 0 ? "unprobed" :
				vic_available() ? "available" : "unavailable",
				vic2d_opted_in() ? "open" : "closed",
				g_n_blt, g_n_rot, g_n_phy);
		return 0;
	}
	return -1;
}

int32_t bsp_g2d_fill(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig, int32_t argb_w, int32_t argb_h,
			int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
	return arch_g2d_fill(argb, argb_phy, contig, argb_w, argb_h, x, y, w, h, color);
}

int32_t bsp_g2d_fill_alpha(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig, int32_t argb_w, int32_t argb_h,
			int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
	uint32_t a = (color >> 24) & 0xffu;
	if (a == 0)
		return G2D_OK;
	if (a == 0xffu)
		return bsp_g2d_fill(argb, argb_phy, contig, argb_w, argb_h, x, y, w, h, color);
	/* fractional source-over: exact arch blend math, CPU only for now */
	return arch_g2d_fill_alpha(argb, argb_w, argb_h, x, y, w, h, color);
}

int32_t bsp_g2d_fill_triangle(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig, int32_t argb_w, int32_t argb_h,
			int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, uint32_t color) {
	if (((color >> 24) & 0xffu) == 0)
		return G2D_OK;
	return arch_g2d_fill_triangle(argb, argb_phy, contig, argb_w, argb_h,
			x1, y1, x2, y2, x3, y3, 0, 0, 0, 0, color);
}

int32_t bsp_g2d_blt(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
			int32_t sx, int32_t sy, int32_t sw, int32_t sh,
			uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h,
			int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
	if (sw == dw && sh == dh && vic_worth(sw, sh, VIC_MIN_COPY_PX) &&
			vic_available() &&
			vic_buf_ok(src_phy, src_contig, src_w) &&
			vic_buf_ok(dst_phy, dst_contig, dst_w) &&
			vic_rect_ok(sx, sy, sw, sh) && vic_rect_ok(dx, dy, dw, dh)) {
		int rc = bsp_vic_blit(src_phy, (uint32_t)src_w * 4u, src_w, src_h,
				sx, sy, sw, sh,
				dst_phy, (uint32_t)dst_w * 4u, dst_w, dst_h, dx, dy, 0);
		if (rc == 0) {
			__atomic_fetch_add(&g_n_blt, 1, __ATOMIC_RELAXED);
			return G2D_OK;
		}
		if (rc == -3)
			return vic_wedge();
	}
	/* scaled blits and declines: the NEON engine handles any geometry */
	return arch_g2d_blt(argb_src, src_phy, src_contig, src_w, src_h, sx, sy, sw, sh,
			argb_dst, dst_phy, dst_contig, dst_w, dst_h, dx, dy, dw, dh);
}

int32_t bsp_g2d_blt_alpha(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
			int32_t sx, int32_t sy, int32_t sw, int32_t sh,
			uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h,
			int32_t dx, int32_t dy, int32_t dw, int32_t dh, uint8_t alpha) {
	if (alpha == 0)
		return G2D_OK;
	/* straight-alpha source-over with (src_a*global)>>8 and /255 rounding:
	   the VIC blender's 10-bit pipeline is not validated against the arch
	   integer math, so every global alpha stays on the exact CPU path. */
	return arch_g2d_blt_alpha(argb_src, src_phy, src_contig, src_w, src_h, sx, sy, sw, sh,
			argb_dst, dst_phy, dst_contig, dst_w, dst_h, dx, dy, dw, dh, alpha);
}

int32_t bsp_g2d_scale_to(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
			uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h) {
	/* the contract is corner-preserving Q15 nearest; the VIC polyphase
	   scaler samples differently.  Exact CPU mapping until validated. */
	return arch_g2d_scale_to(argb_src, src_phy, src_contig, src_w, src_h,
			argb_dst, dst_phy, dst_contig, dst_w, dst_h);
}

int32_t bsp_g2d_rotated_size(int32_t src_w, int32_t src_h, int32_t degree,
			int32_t* dst_w, int32_t* dst_h) {
	return arch_g2d_rotated_size(src_w, src_h, degree, dst_w, dst_h);
}

int32_t bsp_g2d_rotate(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
			uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h, int32_t degree) {
	degree = ((degree % 360) + 360) % 360;
	if ((degree == 0 || degree == 90 || degree == 180 || degree == 270) &&
			vic_worth(src_w, src_h, VIC_MIN_COPY_PX) &&
			vic_available() && (degree == 0 || bsp_vic_2d_can_rotate()) &&
			vic_buf_ok(src_phy, src_contig, src_w) &&
			vic_buf_ok(dst_phy, dst_contig, dst_w)) {
		/* whole-surface right-angle rotate: flip/transpose in the output
		   buffer, no scaling.  dst geometry must match the rotated size
		   exactly: the CPU engine owns the transparent padding of a
		   larger dst. */
		int32_t rw = (degree == 90 || degree == 270) ? src_h : src_w;
		int32_t rh = (degree == 90 || degree == 270) ? src_w : src_h;
		if (dst_w == rw && dst_h == rh) {
			int rc = bsp_vic_blit(src_phy, (uint32_t)src_w * 4u, src_w, src_h,
					0, 0, src_w, src_h,
					dst_phy, (uint32_t)dst_w * 4u, dst_w, dst_h, 0, 0, degree);
			if (rc == 0) {
				__atomic_fetch_add(&g_n_rot, 1, __ATOMIC_RELAXED);
				return G2D_OK;
			}
			if (rc == -3)
				return vic_wedge();
		}
	}
	/* arbitrary angles, in-place, packed pitches: exact CPU mapping */
	return arch_g2d_rotate(argb_src, src_phy, src_contig, src_w, src_h,
			argb_dst, dst_phy, dst_contig, dst_w, dst_h, degree);
}

int32_t bsp_g2d_blt_phy(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
			int32_t sx, int32_t sy, int32_t sw, int32_t sh,
			ewokos_addr_t dst_phy, uint32_t dst_size, int32_t dst_w, int32_t dst_h,
			uint32_t dst_pitch,
			int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
	(void)argb_src;
	if (!vic_available())
		return G2D_ERR_NOT_SUPPORTED;
	if (sw != dw || sh != dh)
		return G2D_ERR_FAILED;   /* no scaled scan-out push yet */
	if (!vic_buf_ok(src_phy, src_contig, src_w) || dst_phy == 0 ||
			(dst_phy & (VIC_ALIGN - 1)) != 0 || (dst_pitch & (VIC_ALIGN - 1)) != 0 ||
			!vic_rect_ok(sx, sy, sw, sh) || !vic_rect_ok(dx, dy, dw, dh))
		return G2D_ERR_FAILED;
	/* the engine is given the whole dst surface, so all of it must be
	   inside the caller's physical range, not just the landing rect */
	if (dst_w <= 0 || dst_h <= 0 ||
			(uint64_t)dst_h * dst_pitch > dst_size)
		return G2D_ERR_FAILED;
	int rc = bsp_vic_blit_xrgb(src_phy, (uint32_t)src_w * 4u, src_w, src_h,
			sx, sy, sw, sh, dst_phy, dst_pitch, dst_w, dst_h, dx, dy, 0);
	if (rc == 0) {
		__atomic_fetch_add(&g_n_phy, 1, __ATOMIC_RELAXED);
		return G2D_OK;
	}
	if (rc == -3)
		return vic_wedge();
	return G2D_ERR_FAILED;   /* declined: displayd runs its memcpy flush */
}

int32_t bsp_g2d_rotate_phy(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
		int32_t sx, int32_t sy, int32_t sw, int32_t sh,
		ewokos_addr_t dst_phy, uint32_t dst_size, int32_t dst_w, int32_t dst_h,
		uint32_t dst_pitch,
		int32_t dx, int32_t dy, int32_t degree) {
	(void)argb_src;
	if (!vic_available())
		return G2D_ERR_NOT_SUPPORTED;
	degree = ((degree % 360) + 360) % 360;
	if (degree != 0 && degree != 90 && degree != 180 && degree != 270)
		return G2D_ERR_FAILED;
	/* not the sticky -2: displayd shares one latch with blit_to_phy,
	   which stays accelerated when only rotation failed its selftest */
	if (degree != 0 && !bsp_vic_2d_can_rotate())
		return G2D_ERR_FAILED;
	if (!vic_buf_ok(src_phy, src_contig, src_w) || dst_phy == 0 ||
			(dst_phy & (VIC_ALIGN - 1)) != 0 || (dst_pitch & (VIC_ALIGN - 1)) != 0 ||
			!vic_rect_ok(sx, sy, sw, sh) || !vic_rect_ok(dx, dy, sw, sh))
		return G2D_ERR_FAILED;
	int32_t rw = (degree == 90 || degree == 270) ? sh : sw;
	int32_t rh = (degree == 90 || degree == 270) ? sw : sh;
	if (dst_w <= 0 || dst_h <= 0 || dx + rw > dst_w || dy + rh > dst_h)
		return G2D_ERR_FAILED;
	if ((uint64_t)dst_h * dst_pitch > dst_size)
		return G2D_ERR_FAILED;
	int rc = bsp_vic_blit_xrgb(src_phy, (uint32_t)src_w * 4u, src_w, src_h,
			sx, sy, sw, sh, dst_phy, dst_pitch, dst_w, dst_h, dx, dy, degree);
	if (rc == 0) {
		__atomic_fetch_add(&g_n_phy, 1, __ATOMIC_RELAXED);
		return G2D_OK;
	}
	if (rc == -3)
		return vic_wedge();
	return G2D_ERR_FAILED;
}

int32_t bsp_g2d_gaussian_blur(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig,
			uint32_t* tmp, ewokos_addr_t tmp_phy, uint8_t tmp_contig,
			int32_t argb_w, int32_t argb_h,
			int32_t rect_x, int32_t rect_y,
			int32_t rect_w, int32_t rect_h,
			int32_t radius) {
	/* VPI documents Gaussian Filter as CPU/KUDA/PVA only — VIC has no
	   gaussian path.  The NEON engine blurs in place (tmp unused). */
	(void)tmp; (void)tmp_phy; (void)tmp_contig;
	return arch_g2d_gaussian(argb, argb_phy, contig, argb_w, argb_h,
			rect_x, rect_y, rect_w, rect_h, radius);
}