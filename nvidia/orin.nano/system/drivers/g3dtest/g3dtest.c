/*
 * g3dtest - console end-to-end test of the portablegl GPU path on Orin.
 *
 * Does what portablegl does for a mesh program, without a window: "compile"
 * each fixed geometry kernel pair through the machine pglcompiler back end,
 * serialize + upload the program to g2dd, stage a scene in contiguous shm
 * canvases, patch the uniform block by name and draw.  The framebuffer and
 * depth canvas are then compared with a CPU run of the same kernel source
 * (libs/ga10b/kernels/pgl_geom.cu) driven through the same parameter
 * derivation the daemon uses.  One scenario per kernel variant.
 *
 * Needs g2dd running with the GPU opted in:
 *   echo GA10B_ENABLE=1 > /tmp/ga10b.cfg
 *   /bin/ipcserv /drivers/orin.nano/g2dd /dev/g2d
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ewoksys/klog.h>
#include <ewoksys/kernel_tic.h>

#include <g2dclient/g2dclient.h>
#include <pglcompiler.h>
#include <geom_raster_cs.h>
#include <ga10b/ga10b_pgl.h>

#define PGL_SIM
#include "../../libs/ga10b/kernels/pgl_geom.cu"

struct pgl_sim_dim3 pgl_sim_threadIdx, pgl_sim_blockIdx, pgl_sim_blockDim;

#define T_W         640u
#define T_H         480u
#define T_NPIX      (T_W * T_H)
#define T_SMALL     2000u
#define T_TRIS      (12u + T_SMALL)
#define T_VERTS     (T_TRIS * 3u)
#define T_CLEAR_GL  0xff302010u      /* 0xAABBGGRR */
#define T_CLEAR_FB  0xff102030u      /* 0xAARRGGBB */
#define T_FRAMES    30u
#define T_TEX       64u
#define T_NINST     4u
#define T_CAP       255u

/* ---- scene: one cube + scattered small triangles, with every attribute
 * any variant needs ---- */
typedef struct {
	float pos[3], nrm[3], rgba[4], uv[2];
	float inst;
} vert_t;

static vert_t g_v[T_VERTS];
static float g_pr[16];               /* projection * rotation, column major */

static uint32_t lcg_state = 0x2468aceu;
static float lcg(void) {
	lcg_state = lcg_state * 1664525u + 1013904223u;
	return (float)(lcg_state >> 8) * (1.0f / 16777216.0f);
}

static void unit(float* v) {
	float l = pgl_sim_sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l < 1e-6f) { v[0] = 0; v[1] = 0; v[2] = 1; return; }
	v[0] /= l; v[1] /= l; v[2] /= l;
}

/* rotation (30deg about Y then 20deg about X), translation (0,0,-4) in the
 * last column: the model-view matrix, column major */
static const float g_mv[16] = {
	0.8660254f, 0.3420201f * 0.5f, -0.9396926f * 0.5f, 0,
	0, 0.9396926f, 0.3420201f, 0,
	0.5f, -0.3420201f * 0.8660254f, 0.9396926f * 0.8660254f, 0,
	0, 0, -4.0f, 1,
};

static void mat_mul(float* o, const float* a, const float* b) {
	for (int col = 0; col < 4; col++)
		for (int row = 0; row < 4; row++) {
			float v = 0;
			for (int k = 0; k < 4; k++)
				v += a[k * 4 + row] * b[col * 4 + k];
			o[col * 4 + row] = v;
		}
}

static void scene_build(void) {
	static const float cv[8][3] = {
		{ -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
		{ -1, -1, 1 },  { 1, -1, 1 },  { 1, 1, 1 },  { -1, 1, 1 },
	};
	static const uint8_t ci[36] = {
		0, 2, 1, 0, 3, 2,  4, 5, 6, 4, 6, 7,  0, 1, 5, 0, 5, 4,
		3, 6, 2, 3, 7, 6,  0, 4, 7, 0, 7, 3,  1, 2, 6, 1, 6, 5,
	};
	const float f = 1.7320508f, aspect = (float)T_W / (float)T_H;
	const float n = 0.5f, fa = 20.0f;
	const float proj[16] = { f / aspect, 0, 0, 0, 0, f, 0, 0,
	                         0, 0, (fa + n) / (n - fa), -1,
	                         0, 0, 2.0f * fa * n / (n - fa), 0 };
	mat_mul(g_pr, proj, g_mv);

	uint32_t v = 0;
	for (uint32_t i = 0; i < 36; i++, v++) {
		const float* p = cv[ci[i]];
		for (int k = 0; k < 3; k++) {
			g_v[v].pos[k] = p[k];
			g_v[v].nrm[k] = p[k];
			g_v[v].rgba[k] = p[k] * 0.5f + 0.5f;
		}
		g_v[v].rgba[3] = 0.6f;
		g_v[v].uv[0] = p[0] * 0.5f + 0.5f;
		g_v[v].uv[1] = p[1] * 0.5f + 0.5f;
	}
	for (uint32_t t = 0; t < T_SMALL; t++) {
		float cx = lcg() * 5.0f - 2.5f, cy = lcg() * 4.0f - 2.0f;
		float cz = lcg() * 5.0f - 2.5f;
		for (uint32_t k = 0; k < 3; k++, v++) {
			g_v[v].pos[0] = cx + lcg() * 0.3f - 0.15f;
			g_v[v].pos[1] = cy + lcg() * 0.3f - 0.15f;
			g_v[v].pos[2] = cz + lcg() * 0.3f - 0.15f;
			for (int c = 0; c < 3; c++) {
				g_v[v].nrm[c] = lcg() - 0.5f;
				g_v[v].rgba[c] = lcg();
			}
			g_v[v].rgba[3] = 0.3f + lcg() * 0.7f;
			g_v[v].uv[0] = lcg();
			g_v[v].uv[1] = lcg();
		}
	}
	for (v = 0; v < T_VERTS; v++) {
		unit(g_v[v].nrm);
		g_v[v].inst = (float)((v / 3u) % T_NINST);
	}
}

/* ---- scenarios ---- */
enum { ATTR1_NONE, ATTR1_RGB, ATTR1_RGBA, ATTR1_NRM };
enum { CTEX_NONE, CTEX_TEX, CTEX_INST36, CTEX_INST40 };

typedef struct {
	const char* name;
	const char* xform_src;
	const char* raster_src;
	uint32_t kind, tpw;
	uint32_t pos_floats;         /* floats per vertex in the pos stream */
	int attr1, ctex;
	uint32_t blend, depth_write, cull;
	int scissor;
} scenario_t;

static const scenario_t g_scen[] = {
	{ "colour", geom_xform_color_cs_glsl, geom_raster_cs_glsl,
	  GA10B_PGL_KIND_GOURAUD, 256, 3, ATTR1_RGB, CTEX_NONE, 0, 1, 0, 0 },
	{ "colour+blend+cull", geom_xform_color_cs_glsl, geom_raster_cs_glsl,
	  GA10B_PGL_KIND_GOURAUD, 256, 3, ATTR1_RGBA, CTEX_NONE, 1, 0, 1, 0 },
	{ "lit", geom_xform_cs_glsl, geom_raster_cs_glsl,
	  GA10B_PGL_KIND_GOURAUD, 256, 3, ATTR1_NRM, CTEX_NONE, 0, 1, 0, 0 },
	{ "textured", geom_xform_tex_cs_glsl, geom_raster_tex_cs_glsl,
	  GA10B_PGL_KIND_TEX, 0, 6, ATTR1_NONE, CTEX_TEX, 0, 1, 0, 0 },
	{ "instanced", geom_xform_inst_cs_glsl, geom_raster_cs_glsl,
	  GA10B_PGL_KIND_INST, 256, 4, ATTR1_NRM, CTEX_INST36, 0, 1, 2, 0 },
	{ "instanced-lit", geom_xform_instlit_cs_glsl, geom_raster_cs_glsl,
	  GA10B_PGL_KIND_INST, 256, 4, ATTR1_NRM, CTEX_INST40, 2, 1, 0, 0 },
	{ "texcol+scissor", geom_xform_texcol_cs_glsl, geom_raster_texcol_cs_glsl,
	  GA10B_PGL_KIND_TEXCOL, 0, 10, ATTR1_NONE, CTEX_TEX, 0, 0, 0, 1 },
};
#define T_NSCEN (sizeof(g_scen) / sizeof(g_scen[0]))

/* shm canvases shared by every scenario */
static int fb_shm, z_shm, pos_shm, col_shm, un_shm, ct_shm;
static uint32_t *fb, *z, *un, *ct;
static float *posf, *colf;
#define T_POS_BYTES  (T_VERTS * 40u)
#define T_COL_BYTES  (T_VERTS * 16u)
#define T_UN_WORDS   256u
#define T_CT_BYTES   (T_TEX * T_TEX * 4u)

/* portablegl's patch_region: write by name into one region of the blob */
static void patch(const struct pglc_stage* st, uint32_t base, const char* name,
		const void* val, uint32_t nc) {
	for (uint32_t i = 0; i < st->num_uniform_bindings; i++) {
		const struct pglc_uniform_binding* ub = &st->uniform_bindings[i];
		if (strcmp(ub->name, name) != 0)
			continue;
		uint32_t span = ub->num_components < nc ? ub->num_components : nc;
		memcpy(un + base + ub->dword_offset, val, span * 4u);
	}
}

static uint32_t now_usec(void) {
	uint64_t nsec = 0;
	kernel_tic_nsec(&nsec);
	return (uint32_t)(nsec / 1000u);
}

static int absdiff(int a, int b) { return a > b ? a - b : b - a; }

/* stage the vertex streams, sampler unit 0 and the uniform block */
static void stage(const scenario_t* sc, const struct pglc_stage* xst,
		const struct pglc_stage* rst) {
	for (uint32_t v = 0; v < T_VERTS; v++) {
		float* p = posf + v * sc->pos_floats;
		memcpy(p, g_v[v].pos, 12);
		if (sc->pos_floats == 4)
			p[3] = g_v[v].inst;
		if (sc->pos_floats >= 6) {
			p[3] = g_v[v].uv[0];
			p[4] = g_v[v].uv[1];
			p[5] = 0;
		}
		if (sc->pos_floats == 10) {
			memcpy(p + 5, g_v[v].rgba, 16);
			p[9] = 0;
		}
		if (sc->attr1 == ATTR1_RGB)
			memcpy(colf + v * 3, g_v[v].rgba, 12);
		else if (sc->attr1 == ATTR1_RGBA)
			memcpy(colf + v * 4, g_v[v].rgba, 16);
		else if (sc->attr1 == ATTR1_NRM)
			memcpy(colf + v * 3, g_v[v].nrm, 12);
	}
	if (sc->ctex == CTEX_TEX) {
		for (uint32_t y = 0; y < T_TEX; y++)
			for (uint32_t x = 0; x < T_TEX; x++) {
				uint32_t chk = ((x >> 3) ^ (y >> 3)) & 1u;
				ct[y * T_TEX + x] = ((chk ? 0xffu : 0x90u) << 24) |
						((x * 4u) << 16) | ((y * 4u) << 8) |
						(chk ? 0xe0u : 0x20u);
			}
	} else if (sc->ctex != CTEX_NONE) {
		uint32_t rec = sc->ctex == CTEX_INST36 ? 36u : 40u;
		float* g = (float*)ct;
		for (uint32_t i = 0; i < T_NINST; i++, g += rec) {
			/* each instance: the scene shifted sideways, own material */
			float tr[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
			                 (float)i * 0.4f - 0.6f, (float)i * 0.2f - 0.3f,
			                 0, 1 };
			mat_mul(g, g_pr, tr);
			memcpy(g + 16, g_mv, 64);
			g[32] = 0.3f + 0.2f * (float)i;
			g[33] = 0.9f - 0.2f * (float)i;
			g[34] = 0.5f;
			g[35] = 0;
			if (rec == 40u) {
				g[35] = 0.05f * (float)i;        /* emissive */
				g[36] = 0.4f + 0.15f * (float)i; /* alpha */
				g[37] = g[38] = g[39] = 0;
			}
		}
	}

	uint32_t rb = xst->uniform_blob_dwords;
	const float wh[2] = { (float)T_W, (float)T_H };
	const float material[4] = { 0.8f, 0.35f, 0.2f, 1.0f };
	const float light_pos[4] = { 2.0f, 3.0f, 4.0f, 1.0f };
	const float light_dir[4] = { 0.3f, 0.5f, 0.8f, 0.0f };
	const float shade[4] = { 0.2f, 0.7f, 0.5f, 0.0f };
	const int32_t scis[4] = { 100, 60, 420, 330 };
	uint32_t one;
	memset(un, 0, T_UN_WORDS * 4u);
	patch(xst, 0, "uMVP", g_pr, 16);
	patch(xst, 0, "uModelView", g_mv, 16);
	patch(xst, 0, "uNormalMat", g_mv, 16);
	patch(xst, 0, "uMaterial", material, 4);
	patch(xst, 0, "uLightEye",
			sc->kind == GA10B_PGL_KIND_INST ? light_dir : light_pos, 4);
	patch(xst, 0, "uShade", shade, 4);
	patch(xst, 0, "uViewportWH", wh, 2);
	patch(rst, rb, "uViewportWH", wh, 2);
	one = T_VERTS;  patch(xst, 0, "uVertCount", &one, 1);
	one = sc->attr1 == ATTR1_RGBA; patch(xst, 0, "uColAlpha", &one, 1);
	one = T_W;      patch(rst, rb, "uFbStride", &one, 1);
	one = 0;        patch(rst, rb, "uTriFirst", &one, 1);
	one = T_TRIS;   patch(rst, rb, "uTriCount", &one, 1);
	patch(rst, rb, "uBlendMode", &sc->blend, 1);
	patch(rst, rb, "uDepthWrite", &sc->depth_write, 1);
	patch(rst, rb, "uCullSign", &sc->cull, 1);
	one = T_TEX;    patch(rst, rb, "uTexW", &one, 1);
	patch(rst, rb, "uTexH", &one, 1);
	if (sc->scissor)
		patch(rst, rb, "uScissor", scis, 4);
}

/* the frame as the daemon computes it, run sequentially on the CPU */
static int reference(const scenario_t* sc, const struct pglc_stage* xst,
		const struct pglc_stage* rst, uint32_t* r_fb, uint32_t* r_z,
		uint32_t* maxbin) {
	const uint32_t tx = (T_W + 15u) / 16u, ty = (T_H + 15u) / 16u;
	float* vout = malloc(T_VERTS * 48u);
	float* trec = malloc(T_TRIS * 160u);
	uint32_t* cnt = calloc(tx * ty, 4u);
	uint32_t* list = malloc(tx * ty * T_CAP * 4u);
	const uint32_t* ru = un + xst->uniform_blob_dwords;
	uint32_t bp[GA10B_PGL_BP_WORDS];
	if (!vout || !trec || !cnt || !list ||
	    ga10b_pgl_fill_bp(bp, rst->code[0], ru, tx, ty, T_CAP, 1,
	                      T_CLEAR_FB) != 0)
		return -1;

	pgl_sim_blockDim.x = 64;
	for (uint32_t v = 0; v < T_VERTS; v++) {
		pgl_sim_blockIdx.x = v / 64u;
		pgl_sim_threadIdx.x = v % 64u;
		switch (xst->code[0]) {
		case GA10B_PGL_K_XFORM_COLOR: pgl_xform_color(vout, posf, colf, un); break;
		case GA10B_PGL_K_XFORM_LIT: pgl_xform_lit(vout, posf, colf, un); break;
		case GA10B_PGL_K_XFORM_TEX: pgl_xform_tex(vout, posf, un); break;
		case GA10B_PGL_K_XFORM_TEXCOL: pgl_xform_texcol(vout, posf, un); break;
		case GA10B_PGL_K_XFORM_INST:
			pgl_xform_inst(vout, posf, colf, (const float*)ct, un); break;
		case GA10B_PGL_K_XFORM_INSTLIT:
			pgl_xform_instlit(vout, posf, colf, (const float*)ct, un); break;
		default: return -1;
		}
	}
	pgl_sim_blockDim.x = 256;
	for (uint32_t t = 0; t < T_TRIS; t++) {
		pgl_sim_blockIdx.x = t / 256u;
		pgl_sim_threadIdx.x = t % 256u;
		pgl_tri_setup(trec, cnt, list, vout, ru, bp);
	}
	for (uint32_t t = 0; t < tx * ty; t++) {
		pgl_sim_blockIdx.x = t / 256u;
		pgl_sim_threadIdx.x = t % 256u;
		pgl_bin_sort(cnt, list, bp);
	}
	for (uint32_t by = 0; by < ty; by++)
		for (uint32_t bx = 0; bx < tx; bx++)
			for (uint32_t i = 0; i < 256u; i++) {
				pgl_sim_blockIdx.x = bx;
				pgl_sim_blockIdx.y = by;
				pgl_sim_threadIdx.x = i % 16u;
				pgl_sim_threadIdx.y = i / 16u;
				pgl_raster_tile(r_fb, r_z, trec, cnt, list, ct, ru, bp);
			}
	*maxbin = 0;
	for (uint32_t t = 0; t < tx * ty; t++)
		if (cnt[t] > *maxbin)
			*maxbin = cnt[t];
	free(vout); free(trec); free(cnt); free(list);
	return 0;
}

static int run(const scenario_t* sc, uint32_t* r_fb, uint32_t* r_z) {
	/* "compile" + serialize + upload, exactly as pgl_g3d_shader.c does */
	struct pglc_program rs, xs;
	if (!pglc_compile_compute_raw(sc->raster_src, &rs) ||
	    !pglc_compile_compute_raw(sc->xform_src, &xs)) {
		klog("g3dtest: %s: kernel compile declined: %s%s\n", sc->name,
				rs.log, xs.log);
		return -1;
	}
	struct pglc_blob_opts opts;
	memset(&opts, 0, sizeof(opts));
	opts.geom_kind = (uint8_t)sc->kind;
	opts.geom_tpw = sc->tpw;
	uint32_t total = pglc_program_serialize(&rs, &xs, &opts, NULL, 0);
	int blob_shm = 0, handle;
	uint32_t* blob = NULL;
	if (total == 0 || g2d_shm_alloc(total, &blob_shm, &blob) != 0 ||
	    pglc_program_serialize(&rs, &xs, &opts, blob, total) != total) {
		klog("g3dtest: %s: serialize failed (total=%u)\n", sc->name, total);
		return -1;
	}
	g2d_pgl_program_req_t preq;
	g2d_pgl_program_req_init(&preq,
			g2d_canvas(blob_shm, total, total / 4u, 1u, 1));
	handle = g2d_pgl_program(&preq);
	g2d_shm_free(blob);
	if (handle < 0) {
		klog("g3dtest: %s: program upload rc=%d\n", sc->name, handle);
		return -1;
	}

	const struct pglc_stage* xst = &xs.stage[PGLC_STAGE_COMPUTE];
	const struct pglc_stage* rst = &rs.stage[PGLC_STAGE_COMPUTE];
	uint32_t nunif = xst->uniform_blob_dwords + rst->uniform_blob_dwords;
	stage(sc, xst, rst);
	memset(fb, 0x55, T_NPIX * 4u);
	memset(z, 0x55, T_NPIX * 4u);

	g2d_pgl_req_t req;
	g2d_canvas_t col = sc->attr1 == ATTR1_NONE ? g2d_canvas(0, 0, 0, 0, 0)
		: g2d_canvas(col_shm, T_COL_BYTES, T_VERTS, 4u, 1);
	g2d_pgl_draw_req_init(&req,
			g2d_canvas(fb_shm, T_NPIX * 4u, T_W, T_H, 1),
			g2d_canvas(pos_shm, T_POS_BYTES, T_VERTS, 10u, 1), col,
			G2D_PGL_MODE_TRIANGLES, T_VERTS, G2D_PGL_FMT_RGBA8, T_W * 4u,
			G2D_PGL_FS_SOLID, 0u, 0, G2D_PGL_DEPTH_LESS, 1, 0,
			g2d_canvas(z_shm, T_NPIX * 4u, T_W, T_H, 1), T_W * 4u, 1,
			T_CLEAR_GL, 1.0f);
	g2d_pgl_draw_req_set_shader(&req, handle, un_shm, nunif, 1);
	if (sc->ctex == CTEX_TEX) {
		req.ctex[0] = g2d_canvas(ct_shm, T_CT_BYTES, T_TEX, T_TEX, 1);
		req.ctex_w[0] = T_TEX;
		req.ctex_h[0] = T_TEX;
		req.ctex_stride[0] = T_TEX * 4u;
	} else if (sc->ctex != CTEX_NONE) {
		uint32_t floats = T_NINST * (sc->ctex == CTEX_INST36 ? 36u : 40u);
		req.ctex[0] = g2d_canvas(ct_shm, T_CT_BYTES, floats, 1u, 1);
		req.ctex_w[0] = floats;
		req.ctex_h[0] = 1u;
		req.ctex_stride[0] = floats * 4u;
	}
	int rc = g2d_pgl_draw(&req);
	if (rc != G2D_OK) {
		klog("g3dtest: %s: draw rc=%d\n", sc->name, rc);
		return -1;
	}

	uint32_t maxbin = 0;
	if (reference(sc, xst, rst, r_fb, r_z, &maxbin) != 0) {
		klog("g3dtest: %s: reference failed\n", sc->name);
		return -1;
	}
	int use_depth = sc->kind != GA10B_PGL_KIND_TEXCOL;
	uint32_t covered = 0, cbad = 0, zbad = 0, uncleared = 0;
	for (uint32_t i = 0; i < T_NPIX; i++) {
		uint32_t g = fb[i], r = r_fb[i];
		if (g == 0x55555555u)
			uncleared++;
		if (r != T_CLEAR_FB)
			covered++;
		if (absdiff((int)(g >> 24), (int)(r >> 24)) > 2 ||
		    absdiff((int)(g >> 16 & 255u), (int)(r >> 16 & 255u)) > 2 ||
		    absdiff((int)(g >> 8 & 255u), (int)(r >> 8 & 255u)) > 2 ||
		    absdiff((int)(g & 255u), (int)(r & 255u)) > 2)
			cbad++;
		if (use_depth) {
			uint32_t dz = z[i] > r_z[i] ? z[i] - r_z[i] : r_z[i] - z[i];
			if (dz > 64u)
				zbad++;
		}
	}

	/* frame time through the whole client -> g2dd -> GPU path */
	uint32_t t0 = now_usec();
	for (uint32_t f = 0; f < T_FRAMES; f++)
		if (g2d_pgl_draw(&req) != G2D_OK) {
			klog("g3dtest: %s: timed draw %u failed\n", sc->name, f);
			return -1;
		}
	uint32_t us = (now_usec() - t0) / T_FRAMES;

	uint32_t budget = T_NPIX / 100u;
	int ok = uncleared == 0 && covered > T_NPIX / 50u && cbad <= budget &&
	         zbad <= budget;
	klog("g3dtest: %-18s %s covered=%u colour_bad=%u depth_bad=%u "
			"fullest_bin=%u frame=%u us\n", sc->name, ok ? "PASS" : "FAIL",
			covered, cbad, zbad, maxbin, us);
	pglc_program_free(&rs);
	pglc_program_free(&xs);
	return ok ? 0 : -1;
}

/* `g3dtest map`: more distinct canvases than the daemon has mapping slots
 * (64), of two sizes, each cleared on the GPU and read back - twice, the
 * second time in reverse order, so evicted buffers are mapped again and a
 * slot is reused for both a smaller and a larger buffer. */
#define M_BUFS 96u
static int map_stress(void) {
	static int shm[M_BUFS];
	static uint32_t* px[M_BUFS];
	uint32_t bad = 0;
	for (uint32_t i = 0; i < M_BUFS; i++) {
		uint32_t w = (i & 1u) ? 128u : 64u;
		if (g2d_shm_alloc(w * 64u * 4u, &shm[i], &px[i]) != 0) {
			klog("g3dtest: map: shm alloc %u failed\n", i);
			return -1;
		}
	}
	for (uint32_t pass = 0; pass < 2; pass++) {
		for (uint32_t k = 0; k < M_BUFS; k++) {
			uint32_t i = pass ? M_BUFS - 1u - k : k;
			uint32_t w = (i & 1u) ? 128u : 64u;
			uint32_t gl = 0xff000000u | ((pass + 1u) << 16) | (i << 8) | 0x5au;
			g2d_pgl_req_t req;
			memset(px[i], 0, w * 64u * 4u);
			g2d_pgl_clear_req_init(&req,
					g2d_canvas(shm[i], w * 64u * 4u, w, 64u, 1),
					G2D_PGL_FMT_RGBA8, w * 4u, gl, 0);
			int rc = g2d_pgl_clear(&req);
			if (rc != G2D_OK) {
				klog("g3dtest: map: clear %u pass %u rc=%d\n", i, pass, rc);
				return -1;
			}
		}
		/* every buffer must hold its own colour (0xAABBGGRR -> 0xAARRGGBB) */
		for (uint32_t i = 0; i < M_BUFS; i++) {
			uint32_t w = (i & 1u) ? 128u : 64u;
			uint32_t want = 0xff000000u | (0x5au << 16) | (i << 8) |
					(pass + 1u);
			for (uint32_t j = 0; j < w * 64u; j++)
				if (px[i][j] != want) {
					if (bad < 4)
						klog("g3dtest: map: buf %u pass %u word %u = %08x "
								"want %08x\n", i, pass, j, px[i][j], want);
					bad++;
					break;
				}
		}
	}
	klog("g3dtest: map stress %s (%u canvases x 2 passes, %u wrong)\n",
			bad ? "FAIL" : "PASS", M_BUFS, bad);
	return bad ? -1 : 0;
}

int main(int argc, char** argv) {
	uint32_t ver = 0;
	if (has_g2d() != 0) {
		klog("g3dtest: no g2dd (/dev/g2d)\n");
		return 1;
	}
	int vr = g2d_get_version(&ver);
	klog("g3dtest: g2d version rc=%d ver=%u supported=%d\n", vr, ver,
			(int)pglc_version_supported(ver));
	if (vr != G2D_OK || !pglc_version_supported(ver))
		return 1;
	if (argc > 1 && strcmp(argv[1], "map") == 0)
		return map_stress() == 0 ? 0 : 1;

	uint32_t *pw, *cw;
	if (g2d_shm_alloc(T_NPIX * 4u, &fb_shm, &fb) != 0 ||
	    g2d_shm_alloc(T_NPIX * 4u, &z_shm, &z) != 0 ||
	    g2d_shm_alloc(T_POS_BYTES, &pos_shm, &pw) != 0 ||
	    g2d_shm_alloc(T_COL_BYTES, &col_shm, &cw) != 0 ||
	    g2d_shm_alloc(T_UN_WORDS * 4u, &un_shm, &un) != 0 ||
	    g2d_shm_alloc(T_CT_BYTES, &ct_shm, &ct) != 0) {
		klog("g3dtest: shm alloc failed\n");
		return 1;
	}
	posf = (float*)pw;
	colf = (float*)cw;
	uint32_t* r_fb = malloc(T_NPIX * 4u);
	uint32_t* r_z = malloc(T_NPIX * 4u);
	if (!r_fb || !r_z)
		return 1;
	scene_build();

	uint32_t fails = 0;
	for (uint32_t i = 0; i < T_NSCEN; i++)
		if (run(&g_scen[i], r_fb, r_z) != 0)
			fails++;
	klog("g3dtest: %s (%u of %u scenarios failed)\n", fails ? "FAIL" : "PASS",
			fails, (uint32_t)T_NSCEN);
	return fails ? 1 : 0;
}
