/*
 * bsp_g3d.c - Orin Nano (GA10B) portablegl 3D offload back end.
 *
 * Forwarder into the GA10B driver library (libs/ga10b, libga10b.a): the
 * compiled-program route runs the two-pass compute rasterizer the other GPU
 * machines use (vertex transform + one thread per triangle), with kernels
 * precompiled offline by nvcc for sm_87 - there is no on-device shader
 * compiler for this GPU, so the Orin pglcompiler back end maps the fixed
 * geometry kernel texts onto those kernels and declines everything else.
 *
 * GATE: the bring-up boots the GPU's secure firmware chain (ACR, PMU RTOS,
 * FECS/GPCCS) from inside g2dd, so it only runs when the environment opts
 * in with GA10B_ENABLE=1 (init.rd export, or a GA10B_ENABLE=1 line in
 * /tmp/ga10b.cfg).  Otherwise every op declines with the sticky -2 and
 * portablegl keeps its CPU rasterizer.
 *
 * WHAT DECLINES: the fixed-function fs_id path (prog_handle < 0; no client
 * sends it), non-ARGB8888 targets, non-triangle modes, and any program the
 * compiler back end could not map to a precompiled kernel pair.
 *
 * Signatures MUST stay byte-identical to the other machines: g2dd.c is
 * shared code and a prototype divergence surfaces only as a link error.
 */

#include <bsp/bsp_g3d.h>
#include <g2dclient/g2dclient.h>   /* G2D_PGL_*, G2D_ERR_* */
#include <ga10b/ga10b_pgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ewoksys/klog.h>

/* 0 = unprobed, 1 = ready, -1 = declined */
static int g_g3d_ready = 0;

/* NAME=1 in the environment (init.rd export) or as a line of
   /tmp/ga10b.cfg, so a serial session can opt in without an init profile */
static int ga10b_flag(const char* name) {
	const char* v = getenv(name);
	if (v != NULL && v[0] == '1' && v[1] == '\0')
		return 1;
	FILE* f = fopen("/tmp/ga10b.cfg", "rb");
	if (f == NULL)
		return 0;
	char buf[128], key[48];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	snprintf(key, sizeof(key), "%s=1", name);
	return strstr(buf, key) != NULL;
}

static int ga10b_opted_in(void) {
	return ga10b_flag("GA10B_ENABLE");
}

static int g3d_probe(void) {
	if (g_g3d_ready == 1 && !ga10b_pgl_ready()) {
		/* a submitted launch never retired */
		g_g3d_ready = -1;
		klog("orin g3d: GPU wedged - 3D offload disabled for this session\n");
	}
	if (g_g3d_ready != 0)
		return g_g3d_ready;
	if (!ga10b_opted_in()) {
		g_g3d_ready = -1;
		return -1;
	}
	g_g3d_ready = (ga10b_pgl_init() == 0) ? 1 : -1;
	ga10b_pgl_set_profile(ga10b_flag("GA10B_PROF"));
	klog("orin g3d: GA10B bring-up %s\n",
			(g_g3d_ready == 1) ? "OK" : "DECLINED");
	return g_g3d_ready;
}

/* GA10B_PGL_E_* use the same numbering as the g2d result codes */
static int32_t pgl_result(int rc) {
	if (rc == GA10B_PGL_OK)
		return G2D_OK;
	if (rc == GA10B_PGL_E_DECLINED)
		return G2D_ERR_FAILED;
	if (rc == GA10B_PGL_E_UNSUPPORTED)
		return G2D_ERR_NOT_SUPPORTED;
	return G2D_ERR_TIMEOUT;
}

/* compiler target bsp_g2d_version() reports: 0 until the GPU is up */
uint32_t bsp_g3d_version(void) {
	return (g3d_probe() == 1) ? GA10B_PGL_VERSION : 0;
}

int32_t bsp_g3d_init(void) {
	/* 3D is optional: g2dd ignores this return value */
	(void)g3d_probe();
	return 0;
}

int32_t bsp_g3d_clear(uint32_t* fb, ewokos_addr_t fb_phy, int32_t w, int32_t h, uint32_t stride, uint32_t fmt,
		uint32_t color, int32_t rb_swap) {
	(void)fb;
	if (g3d_probe() != 1 || fmt != G2D_PGL_FMT_RGBA8)
		return G2D_ERR_NOT_SUPPORTED;
	if (fb_phy == 0 || w <= 0 || h <= 0)
		return G2D_ERR_FAILED;
	/* color is 0xAABBGGRR; the target word is 0xAARRGGBB */
	uint32_t px = color;
	if (!rb_swap)
		px = (color & 0xff00ff00u) | ((color & 0xffu) << 16) |
				((color >> 16) & 0xffu);
	return pgl_result(ga10b_pgl_clear((uint64_t)fb_phy, (uint32_t)w,
			(uint32_t)h, stride, px));
}

int32_t bsp_g3d_draw(ewokos_addr_t fb_phy, int32_t w, int32_t h, uint32_t stride, uint32_t fmt,
		uint32_t mode, uint32_t nverts,
		ewokos_addr_t pos_phy, ewokos_addr_t col_phy, uint32_t fs_id, uint32_t color, int32_t rb_swap,
		uint32_t depth_func, int32_t depth_mask,
		int32_t blend_en, int32_t cull_en,
		ewokos_addr_t z_phy, uint32_t z_stride, uint8_t first_frame,
		uint32_t clear_rgba, float clear_z, const void* ext, const void* tex,
		int32_t prog_handle, const void* unif, uint32_t num_uniforms,
		const void* ctex) {
	ga10b_pgl_draw_t d;
	/* the compiled kernels carry depth / blend / cull state in their
	   uniforms; the legacy fixed-function fields are not consumed */
	(void)fs_id; (void)color; (void)rb_swap; (void)depth_func;
	(void)depth_mask; (void)blend_en; (void)cull_en; (void)clear_z;
	(void)ext; (void)tex;

	if (g3d_probe() != 1 || fmt != G2D_PGL_FMT_RGBA8 || prog_handle < 0)
		return G2D_ERR_NOT_SUPPORTED;
	if (mode != G2D_PGL_MODE_TRIANGLES)
		return G2D_ERR_NOT_SUPPORTED;
	if (fb_phy == 0 || w <= 0 || h <= 0)
		return G2D_ERR_FAILED;

	memset(&d, 0, sizeof(d));
	d.fb_phy      = (uint64_t)fb_phy;
	d.w           = (uint32_t)w;
	d.h           = (uint32_t)h;
	d.stride      = stride;
	d.nverts      = nverts;
	d.pos_phy     = (uint64_t)pos_phy;
	d.col_phy     = (uint64_t)col_phy;
	d.z_phy       = (uint64_t)z_phy;
	d.z_stride    = z_stride;
	d.first_frame = first_frame;
	d.clear_rgba  = clear_rgba;
	/* sampler unit 0 is the textured kinds' image (a linear word array, not
	   a sampled texture) or the instanced kind's record stream */
	if (ctex) {
		const g2d_pgl_ctex_t* c = (const g2d_pgl_ctex_t*)ctex;
		if (c->ctex_phy[0] && c->ctex_tiling[0] == 0 &&
		    c->ctex_depth[0] <= 1) {
			d.ctex_phy    = (uint64_t)c->ctex_phy[0];
			d.ctex_w      = c->ctex_w[0];
			d.ctex_h      = c->ctex_h[0];
			d.ctex_stride = c->ctex_stride[0] ? c->ctex_stride[0]
			                                  : c->ctex_w[0] * 4u;
		}
		/* fullscreen compute software samplers: every bound linear unit
		   (level 0 only; a mip chain stays unbound so the daemon declines
		   the draw to the CPU rather than sampling the wrong level) */
		for (uint32_t u = 0; u < G2D_PGL_MAX_TEX_UNITS &&
		                     u < GA10B_PGL_MAX_TEX; u++) {
			ga10b_pgl_tex_t* t = &d.tex[u];
			if (!c->ctex_phy[u] ||
			    c->ctex_tiling[u] != 0 || c->ctex_levels[u] > 1)
				continue;
			t->phy    = (uint64_t)c->ctex_phy[u];
			t->w      = c->ctex_w[u];
			t->h      = c->ctex_h[u];
			t->stride = c->ctex_stride[u] ? c->ctex_stride[u]
			                              : c->ctex_w[u] * 4u;
			t->depth  = c->ctex_depth[u] ? c->ctex_depth[u] : 1;
			t->face_stride = c->ctex_arrstride64[u] * 64u;
			t->linear = G2D_PGL_CTEX_FILTER_OF(c->ctex_ub_pad[u]) ==
			            G2D_PGL_CTEX_FILTER_LINEAR;
		}
	}
	return pgl_result(ga10b_pgl_draw(prog_handle, &d, (const uint32_t*)unif,
			num_uniforms));
}

int32_t bsp_g3d_program(const void* blob, uint32_t size) {
	if (g3d_probe() != 1)
		return G2D_ERR_NOT_SUPPORTED;
	int handle = ga10b_pgl_program_load(blob, size);
	return handle >= 0 ? (int32_t)handle : pgl_result(handle);
}

int32_t bsp_g3d_program_free(int32_t handle) {
	if (g3d_probe() != 1)
		return G2D_ERR_NOT_SUPPORTED;
	return ga10b_pgl_program_free(handle) == 0
			? G2D_OK : G2D_ERR_NOT_SUPPORTED;
}
